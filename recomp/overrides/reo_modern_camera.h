#pragma once
#include "ps2_runtime.h"
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

// Follow camera, in guest units (Y up, floor at actor Y, roughly centimetres).
// Framing follows modern over-the-shoulder games: the character sits in the left
// third, the eye is near head height and looks almost level, with a fixed FOV
// instead of each room camera's own. REO_MODERN_CAMERA_RIG overrides the values
// as "distance,height,side,lookAhead,lookHeight,fovDegrees,follow" for tuning.
struct Rig {
    float distance = 150.0f;  // behind the character
    float height = 165.0f;    // eye above the character's feet
    float side = 55.0f;       // shoulder offset to the right
    float lookAhead = 450.0f; // look point ahead of the character
    float lookHeight = 125.0f;
    float fovDegrees = 52.0f; // vertical
    float follow = 0.06f;     // yaw smoothing per camera update
};
inline Rig rig;
inline float yaw = 0.0f;
inline bool haveYaw = false;
inline uint16_t yawRoom = 0xffffu;
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

inline void probeFrame(const uint8_t* ram) {
    static unsigned frame = 0;
    if (++frame % 60 != 0 || ram[game.gameState] != 2u || ram[game.localSlot] >= 4u)
        return;
    const uint32_t actor = game.actors + ram[game.localSlot] * game.actorStride;
    std::fprintf(stderr, "[MODCAM-PROBE] f=%u room=%u slot=%u xyz=%.1f,%.1f,%.1f yaw=%d\n", frame,
                 u16(ram, game.room), ram[game.localSlot], f32(ram, actor + 0x38u), f32(ram, actor + 0x3cu),
                 f32(ram, actor + 0x40u), int(int16_t(u16(ram, actor + 0x92u))));
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

inline bool follow(uint8_t* ram) {
    if (ram[game.gameState] != 2u || ram[game.localSlot] >= 4u)
        return false;
    const uint32_t actor = game.actors + ram[game.localSlot] * game.actorStride;
    const float x = f32(ram, actor + 0x38u), y = f32(ram, actor + 0x3cu), z = f32(ram, actor + 0x40u);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    constexpr float tau = 6.28318530718f;
    const float heading = float(int16_t(u16(ram, actor + 0x92u))) * tau / 65536.0f;
    const uint16_t room = u16(ram, game.room);
    if (!haveYaw || room != yawRoom) {
        yaw = heading;
        haveYaw = true;
        yawRoom = room;
    } else {
        float delta = std::remainder(heading - yaw, tau);
        yaw = std::remainder(yaw + delta * rig.follow, tau);
    }
    const float fx = std::sin(yaw), fz = std::cos(yaw);
    // Right of the facing direction (heading = atan2(x, z)).
    const float rx = -fz, rz = fx;
    wf32(ram, game.eye + 0x0u, x - fx * rig.distance + rx * rig.side);
    wf32(ram, game.eye + 0x4u, y + rig.height);
    wf32(ram, game.eye + 0x8u, z - fz * rig.distance + rz * rig.side);
    wf32(ram, game.eye + 0xcu, x + fx * rig.lookAhead + rx * rig.side);
    wf32(ram, game.eye + 0x10u, y + rig.lookHeight);
    wf32(ram, game.eye + 0x14u, z + fz * rig.lookAhead + rz * rig.side);
    applyFov(ram);
    if (probe) {
        static unsigned calls = 0;
        if (++calls % 120 == 1)
            std::fprintf(stderr, "[MODCAM-FOV] scale=%.6f value=%.4f degrees=%.1f\n", f32(ram, game.fovScale),
                         f32(ram, game.fov), rig.fovDegrees);
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

inline bool flag(const char* name) {
    const char* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

inline void install(PS2Runtime& runtime, Addresses addresses = File1) {
    probe = flag("REO_MODERN_CAMERA_PROBE");
    enabled = flag("REO_MODERN_CAMERA");
    if (!probe && !enabled)
        return;
    if (const char* text = std::getenv("REO_MODERN_CAMERA_RIG")) {
        Rig custom = rig;
        if (std::sscanf(text, "%f,%f,%f,%f,%f,%f,%f", &custom.distance, &custom.height, &custom.side,
                        &custom.lookAhead, &custom.lookHeight, &custom.fovDegrees, &custom.follow) == 7 &&
            custom.fovDegrees > 10.0f && custom.fovDegrees < 120.0f && custom.follow > 0.0f &&
            custom.follow <= 1.0f)
            rig = custom;
        else
            std::fprintf(stderr, "[MODCAM-1] ignored REO_MODERN_CAMERA_RIG=%s\n", text);
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
                 "[MODCAM-1] installed follow=%d probe=%d rig=%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.2f\n",
                 int(enabled), int(probe), rig.distance, rig.height, rig.side, rig.lookAhead, rig.lookHeight,
                 rig.fovDegrees, rig.follow);
}
} // namespace reo::modern_camera
