#pragma once
#include "ps2_runtime.h"
#include "runtime/input/input.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// Modern (over-the-shoulder) camera, opt-in with REO_MODERN_CAMERA=1.
// Follows the reo::widescreen model: wrap the original per-frame service and
// edit guest RAM before the original runs. The camera is local only: it never
// changes simulation state shared with co-op peers.
//
// File #1 camera (overlay 0, id 1): the room camera system writes eye
// (0x324138..40) and target (0x324144..4C), Y up, then calls 0x375920, which sets
// the projection and calls the look-at 0x375A50 (view matrix at 0x3240E0). The
// modern camera replaces eye/target inside that call, after the original camera
// logic and before the matrix is built. Movement (original 0x6621F0) reads the
// same eye/target, so walking stays relative to the visible camera.
//
// Orbit: the right stick (keyboard arrows) and the mouse turn the camera around
// the character; it stays where it was left. While L2 is held the right stick
// goes to the game unchanged (original ad-lib) and the camera ignores it. The pad
// feeder (reo_hw_bridge.cpp) calls filterPad and supplies mouse deltas.
//
// REO_MODERN_CAMERA_PROBE=1 logs the File #1 camera block and the local player
// once per second during gameplay (investigation aid, no RAM writes).

void o0_00375920_0x375920(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);

namespace reo::modern_camera {
struct Addresses {
    uint32_t frameService; // original per-frame service
    uint32_t cameraBlock;  // camera block (eye/target floats inside)
    uint32_t actors;       // actor array base
    uint32_t actorStride;
    uint32_t localSlot;    // u8 local player slot
    uint32_t gameState;    // u8, 2 during room gameplay
    uint32_t room;         // u16 current room
    uint32_t eye;          // 3 floats; target follows at +0x0C
    uint32_t overlayBase;  // overlay holding the camera apply function
    uint32_t overlayId;
    uint32_t cameraApply;  // projection + look-at
    uint32_t fov;          // float read by cameraApply; vertical FOV = fov * fovScale radians
    uint32_t fovScale;     // overlay 0 constant multiplying fov
};
inline constexpr Addresses File1{0x19f6c0u, 0x324100u, 0x4a5c30u, 0x10e0u, 0x4badedu, 0x4badd0u,
                                 0x3243dcu, 0x324138u, 0x370000u, 1u, 0x375920u, 0x324180u, 0x38ca80u};

inline Addresses game = File1;
inline bool probe = false;
inline bool enabled = false;

// Camera rig, in guest units (Y up, floor at actor Y, roughly centimetres).
// Framing follows modern over-the-shoulder games: the character sits in the left
// third, the eye is near head height and looks almost level, with a fixed FOV
// instead of each room camera's own. REO_MODERN_CAMERA_RIG overrides the values
// as "distance,height,side,lookAhead,lookHeight,fovDegrees,recenter" for tuning.
struct Rig {
    float distance = 150.0f;  // behind the orbit point
    float height = 165.0f;    // eye above the character's feet at the resting pitch
    float side = 55.0f;       // shoulder offset to the right
    float lookAhead = 450.0f; // look point ahead of the character
    float lookHeight = 125.0f;
    float fovDegrees = 52.0f; // vertical
    float recenter = 0.0f;    // per-update pull back behind the character after idle; 0 = stays put
};
inline Rig rig;

// Orbit input. REO_MODERN_CAMERA_CONTROLS overrides
// "stickYawDegPerSec,stickPitchDegPerSec,mouseDegPerPixel,invertY".
struct Controls {
    float stickYaw = 150.0f;
    float stickPitch = 90.0f;
    float mouse = 0.12f;
    float invertY = 0.0f;
};
inline Controls controls;
inline constexpr float kPitchMin = -35.0f, kPitchMax = 50.0f; // degrees; positive looks down

// Written by the pad feeder thread, consumed on the guest thread.
inline std::atomic<float> stickX{0.0f}, stickY{0.0f}; // -1..1, right/down positive
inline std::atomic<int> mouseDx{0}, mouseDy{0};       // accumulated pixels

inline float yaw = 0.0f, pitch = 0.0f; // radians
inline bool haveYaw = false;
inline uint16_t yawRoom = 0xffffu;
inline std::chrono::steady_clock::time_point lastUpdate{}, lastInput{};
inline void (*originalService)(uint8_t*, R5900Context*, PS2Runtime*) = nullptr;

inline float f32(const uint8_t* ram, uint32_t address) {
    float value;
    std::memcpy(&value, ram + address, 4);
    return value;
}
inline uint16_t u16(const uint8_t* ram, uint32_t address) {
    uint16_t value;
    std::memcpy(&value, ram + address, 2);
    return value;
}
inline bool flag(const char* name) {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}
inline bool active() {
    static const bool on = flag("REO_MODERN_CAMERA");
    return on;
}

// Free room control: no loading, door, cutscene, message or menu (the same guards
// the automated walk uses before steering).
inline bool freeGameplay(const uint8_t* ram) {
    return ram && ram[game.gameState] == 2u && ram[0x4badeeu] == 0 && ram[0x755ec4u] == 0 &&
           ram[0x4badf8u] == 0 && ram[0x4bae8du] == 0 && ram[game.localSlot] < 4u && ram[0x4bae07u] == 0;
}

// Pad feeder hook: route the right stick to the camera during free gameplay,
// except while L2 is held (ad-lib). The game then sees a centred right stick.
inline void filterPad(const uint8_t* ram, reo::DualShock2State& pad) {
    if (!active())
        return;
    if (!freeGameplay(ram) || !(pad.buttons & reo::PS2_L2)) {
        stickX.store(0.0f, std::memory_order_relaxed);
        stickY.store(0.0f, std::memory_order_relaxed);
        return;
    }
    static const char* test = std::getenv("REO_TEST_CAMERA_STICK");
    int rx = pad.right_x, ry = pad.right_y;
    auto axis = [](int raw) {
        const float value = (float(raw) - 128.0f) / 127.0f;
        const float dead = 0.18f;
        if (std::fabs(value) < dead)
            return 0.0f;
        const float scaled = (std::fabs(value) - dead) / (1.0f - dead);
        return std::copysign(std::fmin(scaled, 1.0f), value);
    };
    // Automated tests may supply a deflection while the real stick rests.
    if (test && axis(rx) == 0.0f && axis(ry) == 0.0f)
        std::sscanf(test, "%d,%d", &rx, &ry);
    stickX.store(axis(rx), std::memory_order_relaxed);
    stickY.store(axis(ry), std::memory_order_relaxed);
    pad.right_x = 0x80;
    pad.right_y = 0x80;
}

inline void addMouse(int dx, int dy) {
    mouseDx.fetch_add(dx, std::memory_order_relaxed);
    mouseDy.fetch_add(dy, std::memory_order_relaxed);
}

inline void probeFrame(const uint8_t* ram) {
    static unsigned frame = 0;
    if (++frame % 60 != 0 || ram[game.gameState] != 2u || ram[game.localSlot] >= 4u)
        return;
    const uint32_t actor = game.actors + ram[game.localSlot] * game.actorStride;
    std::fprintf(stderr, "[MODCAM-PROBE] f=%u room=%u slot=%u xyz=%.1f,%.1f,%.1f yaw=%d cam=%.1f/%.1f\n", frame,
                 u16(ram, game.room), ram[game.localSlot], f32(ram, actor + 0x38u), f32(ram, actor + 0x3cu),
                 f32(ram, actor + 0x40u), int(int16_t(u16(ram, actor + 0x92u))), yaw * 57.2957795f,
                 pitch * 57.2957795f);
    for (uint32_t row = 0; row < 0x80u; row += 0x10u) {
        const uint32_t a = game.cameraBlock + row;
        std::fprintf(stderr, "[MODCAM-PROBE]   %06x: %12.3f %12.3f %12.3f %12.3f\n", a, f32(ram, a),
                     f32(ram, a + 4), f32(ram, a + 8), f32(ram, a + 12));
    }
}

inline void wf32(uint8_t* ram, uint32_t address, float value) {
    std::memcpy(ram + address, &value, 4);
}

inline void applyFov(uint8_t* ram) {
    const float scale = f32(ram, game.fovScale);
    if (!std::isfinite(scale) || scale <= 0.0f)
        return;
    const float value = rig.fovDegrees * 0.01745329252f / scale;
    if (std::isfinite(value) && value > 0.0f)
        wf32(ram, game.fov, value);
}

inline float restingPitch() {
    return std::atan2(rig.height - rig.lookHeight, rig.distance + rig.lookAhead);
}

inline bool follow(uint8_t* ram) {
    if (ram[game.gameState] != 2u || ram[game.localSlot] >= 4u)
        return false;
    const uint32_t actor = game.actors + ram[game.localSlot] * game.actorStride;
    const float x = f32(ram, actor + 0x38u), y = f32(ram, actor + 0x3cu), z = f32(ram, actor + 0x40u);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    constexpr float tau = 6.28318530718f, degree = 0.01745329252f;
    const float heading = float(int16_t(u16(ram, actor + 0x92u))) * tau / 65536.0f;
    const uint16_t room = u16(ram, game.room);
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastUpdate).count();
    lastUpdate = now;
    if (!(dt >= 0.0f) || dt > 0.1f)
        dt = 0.0f;
    const float sx = stickX.load(std::memory_order_relaxed), sy = stickY.load(std::memory_order_relaxed);
    const int mx = mouseDx.exchange(0, std::memory_order_relaxed);
    const int my = mouseDy.exchange(0, std::memory_order_relaxed);
    if (!haveYaw || room != yawRoom) {
        // Entering a room: start behind the character at the resting pitch.
        yaw = heading;
        pitch = restingPitch();
        haveYaw = true;
        yawRoom = room;
        lastInput = now;
    } else {
        const float invert = controls.invertY != 0.0f ? -1.0f : 1.0f;
        // Turning right moves the view toward the right vector, i.e. lowers yaw.
        yaw -= (sx * controls.stickYaw * dt + float(mx) * controls.mouse) * degree;
        // Stick/mouse up looks up (lower pitch).
        pitch += invert * (sy * controls.stickPitch * dt + float(my) * controls.mouse) * degree;
        pitch = std::fmin(std::fmax(pitch, kPitchMin * degree), kPitchMax * degree);
        if (sx != 0.0f || sy != 0.0f || mx || my)
            lastInput = now;
        else if (rig.recenter > 0.0f && std::chrono::duration<float>(now - lastInput).count() > 2.0f)
            yaw += std::remainder(heading - yaw, tau) * rig.recenter;
        yaw = std::remainder(yaw, tau);
    }
    const float fx = std::sin(yaw), fz = std::cos(yaw);
    // Right of the facing direction (heading = atan2(x, z)).
    const float rx = -fz, rz = fx;
    // Orbit around the shoulder point; at the resting pitch this matches the rig.
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float dx = fx * cp, dy = -sp, dz = fz * cp;
    const float px = x + rx * rig.side, py = y + rig.height, pz = z + rz * rig.side;
    const float ex = px - dx * rig.distance, ey = py - dy * rig.distance - std::sin(restingPitch()) * rig.distance,
                ez = pz - dz * rig.distance;
    const float reach = rig.distance + rig.lookAhead;
    wf32(ram, game.eye + 0x0u, ex);
    wf32(ram, game.eye + 0x4u, ey);
    wf32(ram, game.eye + 0x8u, ez);
    wf32(ram, game.eye + 0xcu, ex + dx * reach);
    wf32(ram, game.eye + 0x10u, ey + dy * reach);
    wf32(ram, game.eye + 0x14u, ez + dz * reach);
    applyFov(ram);
    if (probe) {
        static unsigned calls = 0;
        if (++calls % 120 == 1)
            std::fprintf(stderr, "[MODCAM-FOV] scale=%.6f value=%.4f degrees=%.1f\n",
                         f32(ram, game.fovScale), f32(ram, game.fov), rig.fovDegrees);
    }
    return true;
}

inline void cameraApply(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    if (ctx->pc == game.cameraApply && enabled)
        follow(ram);
    o0_00375920_0x375920(ram, ctx, runtime);
}

inline void service(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool entry = ctx->pc == game.frameService;
    if (entry && probe)
        probeFrame(ram);
    originalService(ram, ctx, runtime);
}

inline void install(PS2Runtime& runtime, Addresses addresses = File1) {
    probe = flag("REO_MODERN_CAMERA_PROBE");
    enabled = flag("REO_MODERN_CAMERA");
    if (!probe && !enabled)
        return;
    if (const char* text = std::getenv("REO_MODERN_CAMERA_RIG")) {
        Rig custom = rig;
        if (std::sscanf(text, "%f,%f,%f,%f,%f,%f,%f", &custom.distance, &custom.height, &custom.side,
                        &custom.lookAhead, &custom.lookHeight, &custom.fovDegrees, &custom.recenter) == 7 &&
            custom.fovDegrees > 10.0f && custom.fovDegrees < 120.0f && custom.recenter >= 0.0f &&
            custom.recenter <= 1.0f)
            rig = custom;
        else
            std::fprintf(stderr, "[MODCAM-1] ignored REO_MODERN_CAMERA_RIG=%s\n", text);
    }
    if (const char* text = std::getenv("REO_MODERN_CAMERA_CONTROLS")) {
        Controls custom = controls;
        if (std::sscanf(text, "%f,%f,%f,%f", &custom.stickYaw, &custom.stickPitch, &custom.mouse,
                        &custom.invertY) == 4 &&
            custom.stickYaw >= 0.0f && custom.stickPitch >= 0.0f && custom.mouse >= 0.0f)
            controls = custom;
        else
            std::fprintf(stderr, "[MODCAM-1] ignored REO_MODERN_CAMERA_CONTROLS=%s\n", text);
    }
    game = addresses;
    originalService = runtime.lookupFunction(game.frameService);
    if (!originalService || originalService == service)
        throw std::runtime_error("Modern camera frame service unavailable");
    runtime.registerFunction(game.frameService, service);
    if (enabled)
        // Overlay-aware: replaces the function only while overlay 0 is resident.
        runtime.registerOverlayFunction(game.overlayBase, 0x336f574du, 4u, game.overlayId, game.cameraApply,
                                        cameraApply);
    std::fprintf(stderr,
                 "[MODCAM-1] installed follow=%d probe=%d rig=%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.2f "
                 "controls=%.0f,%.0f,%.2f,%.0f\n",
                 int(enabled), int(probe), rig.distance, rig.height, rig.side, rig.lookAhead, rig.lookHeight,
                 rig.fovDegrees, rig.recenter, controls.stickYaw, controls.stickPitch, controls.mouse,
                 controls.invertY);
}
} // namespace reo::modern_camera
