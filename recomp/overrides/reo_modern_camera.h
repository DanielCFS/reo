#pragma once
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/input/input.h"
#include "runtime/gs/performance.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

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
void o2_0065ED70_0x65ed70(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
void o2_00586AA0_0x586aa0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);

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
// Aiming (R1 held) blends to a tighter over-the-shoulder view, as in modern
// remakes; the character then faces where the camera looks.
inline Rig aimRig{95.0f, 160.0f, 50.0f, 600.0f, 140.0f, 40.0f, 0.0f};
inline constexpr float kAimBlendSeconds = 0.10f;
// While aiming, camera pitch beyond these angles aims high/low with the game's
// own up/down aim (digital up/down while the weapon is readied).
inline constexpr float kAimUpPitch = -10.0f, kAimDownPitch = 12.0f;
// Movement facing turns toward camera + stick direction at this rate; the game
// itself keeps a run's direction while the input stays the same.
inline constexpr float kTurnDegPerSec = 720.0f;
inline float aimBlend = 0.0f; // 0 = normal rig, 1 = aim rig
inline float cameraClearance = 1.0f; // 1 = full distance, 0 = pulled in by a wall

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
inline std::atomic<float> moveX{0.0f}, moveY{0.0f};   // left stick as sent, -1..1
inline std::atomic<bool> aimHeld{false};               // R1 held in free gameplay
// Written on the guest thread, read by the feeder for vertical aim.
inline std::atomic<float> aimPitch{0.0f};              // degrees

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

// Free room control: no loading, door, cutscene, message or menu. 0x4BAE07 is
// deliberately not checked: it keeps a scenario event value (3 after the J's Bar
// door cutscene) for the rest of the room, which would disable PC controls.
inline bool freeGameplay(const uint8_t* ram) {
    return ram && ram[game.gameState] == 2u && ram[0x4badeeu] == 0 && ram[0x755ec4u] == 0 &&
           ram[0x4badf8u] == 0 && ram[0x4bae8du] == 0 && ram[game.localSlot] < 4u;
}

inline uint32_t localActor(const uint8_t* ram) {
    return game.actors + ram[game.localSlot] * game.actorStride;
}
// Weapon readied (R1): actor state 2, sub-state 1.
inline bool aiming(const uint8_t* ram) {
    if (!ram || ram[game.localSlot] >= 4u)
        return false;
    const uint32_t actor = localActor(ram);
    return ram[actor + 8u] == 2u && ram[actor + 9u] == 1u;
}

// Free aim (REO_MODERN_CAMERA_FREE_AIM, default on with the modern camera).
// File #1 player actors keep their aim target at +0xCDC (spawn index, 0xFF none;
// spawn record 0x74F250 + i*96, byte +1 = enemy slot) and the vertical aim at
// +0xBC8 (int16, +-0x2000, down positive). The original search o2 0x65ED70 takes
// the nearest valid enemy in any direction (per-character cone 0x71B960 = 360
// degrees) from the local player's distance-sorted list 0x3B2E10 + slot*50 with
// per-enemy {distance, angle, blocked} records at 0x3B41C0 + slot*400. The modern
// camera picks the enemy nearest the crosshair ray instead, keeps it while the
// crosshair stays on it, and without a target aims where the crosshair points
// (o2 0x586AA0 otherwise snaps the vertical aim to three digital levels).
struct AimRay {
    bool valid = false;
    float origin[3]{}, direction[3]{};
};
inline AimRay aimRay;
inline int16_t freeAimPitch = 0;
inline constexpr float kLockConeDeg = 7.0f, kHoldConeDeg = 12.0f;
inline constexpr uint32_t kEnemies = 0x42eb00u, kEnemyStride = 0xd90u, kNoTarget = 0xffu;

inline bool freeAimEnabled() {
    static const bool on = [] {
        const char* value = std::getenv("REO_MODERN_CAMERA_FREE_AIM");
        return !value || std::strcmp(value, "0") != 0;
    }();
    return on;
}
inline uint32_t u32(const uint8_t* ram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, ram + address, 4);
    return value;
}
inline bool guestRange(uint32_t address, uint32_t bytes) {
    return address >= 0x100000u && address < 0x2000000u - bytes;
}
// Point to aim at on an enemy: the body height the original search checks
// (bone table +0x34, entry [+0x551], +0x820 -> +0x34), else waist height.
inline bool enemyPoint(const uint8_t* ram, uint32_t enemy, float point[3]) {
    point[0] = f32(ram, enemy + 0x38u);
    point[1] = f32(ram, enemy + 0x3cu) + 100.0f;
    point[2] = f32(ram, enemy + 0x40u);
    const uint32_t table = u32(ram, enemy + 0x34u);
    if (guestRange(table, 0x900u)) {
        const uint32_t bone = u32(ram, table + ram[enemy + 0x551u] * 4u + 0x820u);
        if (guestRange(bone, 0x40u)) {
            const float y = f32(ram, bone + 0x34u);
            if (std::isfinite(y) && y > point[1] - 160.0f && y < point[1] + 200.0f)
                point[1] = y;
        }
    }
    return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]);
}
inline float rayAngleDeg(const float point[3]) {
    const float v[3] = {point[0] - aimRay.origin[0], point[1] - aimRay.origin[1], point[2] - aimRay.origin[2]};
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1.0f))
        return 180.0f;
    const float c = (v[0] * aimRay.direction[0] + v[1] * aimRay.direction[1] + v[2] * aimRay.direction[2]) / length;
    return std::acos(std::fmin(std::fmax(c, -1.0f), 1.0f)) * 57.2957795f;
}
// The enemy currently targeted by a player actor, or 0.
inline uint32_t currentTarget(const uint8_t* ram, uint32_t actor) {
    const uint8_t spawn = ram[actor + 0xcdcu];
    if (spawn == kNoTarget)
        return 0;
    const uint8_t slot = ram[0x74f251u + spawn * 96u];
    if (slot >= 50u)
        return 0;
    const uint32_t enemy = kEnemies + slot * kEnemyStride;
    return ram[enemy] ? enemy : 0;
}
// Same validity rules as the original search (weapon range, line of sight flag,
// height window), choosing the smallest angle from the crosshair within cone.
inline uint32_t pickTarget(const uint8_t* ram, uint32_t actor, float coneDeg, uint32_t keep = 0) {
    const uint8_t slot = ram[actor + 3u];
    if (!aimRay.valid || ram[actor + 2u] != 0 || slot >= 4u)
        return 0;
    const uint32_t list = 0x3b2e10u + slot * 50u, data = 0x3b41c0u + slot * 400u;
    const uint16_t weapon = u16(ram, actor + 0xca8u);
    const float range = weapon < 256u ? float(ram[0x719a6au + weapon * 68u]) * 200.0f : 0.0f;
    const float height = f32(ram, actor + 0x3cu);
    uint32_t best = 0;
    float bestAngle = coneDeg;
    for (uint32_t i = 0; i < 50u; ++i) {
        const uint8_t index = ram[list + i];
        if (index == kNoTarget)
            break;
        if (index >= 50u)
            continue;
        const uint32_t enemy = kEnemies + index * kEnemyStride, record = data + index * 8u;
        if ((u32(ram, enemy + 0xc94u) & 1u) || ram[record + 6u] || !(f32(ram, record) <= range) ||
            height - f32(ram, enemy + 0x3cu) > 100.0f)
            continue;
        float point[3];
        if (!enemyPoint(ram, enemy, point))
            continue;
        const float angle = rayAngleDeg(point) - (enemy == keep ? kHoldConeDeg - kLockConeDeg : 0.0f);
        if (angle < bestAngle) {
            bestAngle = angle;
            best = enemy;
        }
    }
    return best;
}
// Write a target the way the original search does (+0xCD4/CD6/CD8, vertical aim
// restarts at level when the target changes).
inline void setTarget(uint8_t* ram, uint32_t actor, uint32_t enemy) {
    if (!enemy) {
        ram[actor + 0xcdcu] = uint8_t(kNoTarget);
        return;
    }
    const uint8_t spawn = ram[enemy + 0xb98u];
    if (ram[actor + 0xcdcu] == spawn)
        return;
    const uint32_t index = (enemy - kEnemies) / kEnemyStride;
    const uint16_t angle = u16(ram, 0x3b41c0u + ram[actor + 3u] * 400u + index * 8u + 4u), zero = 0;
    std::memcpy(ram + actor + 0xcd4u, &zero, 2);
    std::memcpy(ram + actor + 0xcd6u, &angle, 2);
    std::memcpy(ram + actor + 0xcd8u, &zero, 2);
    std::memcpy(ram + actor + 0xbc8u, &zero, 2);
    ram[actor + 0xcdcu] = spawn;
}
// Per frame while aiming: keep the target while the crosshair stays near it,
// otherwise lock whatever the crosshair now covers (or nothing).
inline uint32_t maintainTarget(uint8_t* ram, uint32_t actor) {
    const uint32_t current = currentTarget(ram, actor);
    const uint32_t next = pickTarget(ram, actor, kLockConeDeg, current);
    setTarget(ram, actor, next);
    return next;
}

// Pad feeder hook (pressed: active-high PS2 buttons sent to the game).
// During free gameplay the D-pad becomes a camera-relative left-stick direction
// (keyboard WASD reaches the game as D-pad), and while aiming the directions are
// withheld because the camera sets the character's facing. The right stick turns
// the camera except while L2 is held (fixed chat); the game then sees it centred.
inline void filterPad(const uint8_t* ram, reo::DualShock2State& pad, uint16_t& pressed) {
    if (!active())
        return;
    const bool free = freeGameplay(ram);
    static const bool logFree = std::getenv("REO_MODERN_CAMERA_MENU_LOG") != nullptr;
    static int lastFree = -1;
    if (logFree && int(free) != lastFree && ram) {
        lastFree = int(free);
        std::fprintf(stderr, "[MODCAM-FREE] free=%d gs=%u guards=%02x,%02x,%02x,%02x\n", int(free),
                     ram[game.gameState], ram[0x4badeeu], ram[0x755ec4u], ram[0x4badf8u], ram[0x4bae8du]);
    }
    aimHeld.store(free && (pressed & reo::PS2_R1), std::memory_order_relaxed);
    float mx = 0.0f, my = 0.0f;
    if (free) {
        constexpr uint16_t dpad = reo::PS2_UP | reo::PS2_DOWN | reo::PS2_LEFT | reo::PS2_RIGHT;
        const bool stickRests = std::abs(int(pad.left_x) - 128) <= 32 && std::abs(int(pad.left_y) - 128) <= 32;
        if (aiming(ram)) {
            pressed &= uint16_t(~dpad);
            pad.buttons |= dpad;
            pad.left_x = pad.left_y = 0x80;
            const float tilt = aimPitch.load(std::memory_order_relaxed);
            const uint16_t vertical = tilt < kAimUpPitch ? reo::PS2_UP : tilt > kAimDownPitch ? reo::PS2_DOWN : 0;
            pressed |= vertical;
            pad.buttons &= uint16_t(~vertical);
        } else if ((pressed & dpad) && stickRests) {
            const int x = ((pressed & reo::PS2_RIGHT) ? 1 : 0) - ((pressed & reo::PS2_LEFT) ? 1 : 0);
            const int y = ((pressed & reo::PS2_DOWN) ? 1 : 0) - ((pressed & reo::PS2_UP) ? 1 : 0);
            const float scale = (x && y) ? 0.7071f : 1.0f;
            pad.left_x = uint8_t(std::lround(128.0f + 127.0f * scale * float(x)));
            pad.left_y = uint8_t(std::lround(128.0f + 127.0f * scale * float(y)));
            pressed &= uint16_t(~dpad);
            pad.buttons |= dpad;
        }
        if (!aiming(ram)) {
            mx = (float(pad.left_x) - 128.0f) / 127.0f;
            my = (float(pad.left_y) - 128.0f) / 127.0f;
            if (mx * mx + my * my < 0.3f * 0.3f)
                mx = my = 0.0f;
        }
    }
    moveX.store(mx, std::memory_order_relaxed);
    moveY.store(my, std::memory_order_relaxed);
    if (!free || !(pad.buttons & reo::PS2_L2)) {
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
    std::fprintf(stderr,
                 "[MODCAM-PROBE] f=%u room=%u slot=%u xyz=%.1f,%.1f,%.1f yaw=%d cam=%.1f/%.1f "
                 "guards=%02x,%02x,%02x,%02x,%02x free=%d state=%u/%u clear=%.2f\n",
                 frame, u16(ram, game.room), ram[game.localSlot], f32(ram, actor + 0x38u), f32(ram, actor + 0x3cu),
                 f32(ram, actor + 0x40u), int(int16_t(u16(ram, actor + 0x92u))), yaw * 57.2957795f,
                 pitch * 57.2957795f, ram[0x4badeeu], ram[0x755ec4u], ram[0x4badf8u], ram[0x4bae8du],
                 ram[0x4bae07u], int(freeGameplay(ram)), ram[actor + 8u], ram[actor + 9u], cameraClearance);
    for (uint32_t row = 0; row < 0x80u; row += 0x10u) {
        const uint32_t a = game.cameraBlock + row;
        std::fprintf(stderr, "[MODCAM-PROBE]   %06x: %12.3f %12.3f %12.3f %12.3f\n", a, f32(ram, a),
                     f32(ram, a + 4), f32(ram, a + 8), f32(ram, a + 12));
    }
    // REO_MODERN_CAMERA_PROBE=2: append the local actor record to modcam_actor.bin
    // (u32 frame, then actorStride bytes) for offline state diffs.
    static const bool dump = [] {
        const char* value = std::getenv("REO_MODERN_CAMERA_PROBE");
        return value && std::strcmp(value, "2") == 0;
    }();
    if (dump) {
        // Control type and its Triangle/Cross/Circle action masks (0x2343C0 + type*12).
        const uint8_t type = ram[0x3243ecu];
        uint32_t masks[3] = {};
        if (type < 3u)
            std::memcpy(masks, ram + 0x2343c0u + type * 12u, sizeof(masks));
        std::fprintf(stderr, "[MODCAM-PROBE] control type=%u triangle=%08x cross=%08x circle=%08x\n", type,
                     masks[0], masks[1], masks[2]);
        if (std::FILE* out = std::fopen("modcam_actor.bin", "ab")) {
            std::fwrite(&frame, 4, 1, out);
            std::fwrite(ram + actor, 1, game.actorStride, out);
            std::fclose(out);
        }
    }
}

inline void wf32(uint8_t* ram, uint32_t address, float value) {
    std::memcpy(ram + address, &value, 4);
}

inline void applyFov(uint8_t* ram, float degrees) {
    const float scale = f32(ram, game.fovScale);
    if (!std::isfinite(scale) || scale <= 0.0f)
        return;
    const float value = degrees * 0.01745329252f / scale;
    if (std::isfinite(value) && value > 0.0f)
        wf32(ram, game.fov, value);
}

inline float restingPitch() {
    return std::atan2(rig.height - rig.lookHeight, rig.distance + rig.lookAhead);
}

// Wall collision (stage 3) uses the room's own collision walls. The collision
// context at 0x3C2700 points (+0x24) to an array of 56-byte triangles: +0 id,
// +4 three vertices (x, y, z), +0x28 plane normal, +0x34 plane d. They are the
// walls actors collide with (an actor stops 50 units from them), including
// door frames and walls with scenery behind them, where the floor continues.
// The list is copied when the room or the pointer changes; each record must
// have a unit normal and lie on its own plane.
struct WallTriangle {
    float origin[3], edge1[3], edge2[3];
};
inline std::vector<WallTriangle> walls;
inline uint32_t wallsBase = 0;
inline uint16_t wallsRoom = 0xffffu;

inline bool collisionEnabled() {
    static const bool on = [] {
        const char* value = std::getenv("REO_MODERN_CAMERA_COLLISION");
        return !(value && std::strcmp(value, "0") == 0);
    }();
    return on;
}

inline void loadWalls(const uint8_t* ram) {
    uint32_t base = 0;
    std::memcpy(&base, ram + 0x3c2724u, 4);
    const uint16_t room = u16(ram, game.room);
    if (base == wallsBase && room == wallsRoom)
        return;
    wallsBase = base;
    wallsRoom = room;
    walls.clear();
    if (base < 0x100000u || base >= 0x2000000u - 0x38u)
        return;
    for (uint32_t record = base; record + 0x38u <= 0x2000000u && walls.size() < 4096; record += 0x38u) {
        float f[13];
        std::memcpy(f, ram + record + 4u, sizeof(f));
        bool finite = true;
        for (float v : f)
            finite = finite && std::isfinite(v);
        const float length = std::sqrt(f[9] * f[9] + f[10] * f[10] + f[11] * f[11]);
        if (!finite || std::fabs(length - 1.0f) > 0.02f ||
            std::fabs(f[9] * f[0] + f[10] * f[1] + f[11] * f[2] + f[12]) > 2.0f)
            break;
        walls.push_back({{f[0], f[1], f[2]}, {f[3] - f[0], f[4] - f[1], f[5] - f[2]}, {f[6] - f[0], f[7] - f[1], f[8] - f[2]}});
    }
    if (probe)
        std::fprintf(stderr, "[MODCAM-WALLS] room=%u base=%08x walls=%zu\n", room, base, walls.size());
}

// First hit of the segment from -> to against the room walls, as a fraction of
// the segment (1 when clear). Moller-Trumbore, both faces.
inline float firstWallHit(const float from[3], const float to[3]) {
    const float dir[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    float best = 1.0f;
    for (const auto& w : walls) {
        const float* e1 = w.edge1;
        const float* e2 = w.edge2;
        const float p[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-6f)
            continue;
        const float inv = 1.0f / det;
        const float t0[3] = {from[0] - w.origin[0], from[1] - w.origin[1], from[2] - w.origin[2]};
        const float u = (t0[0] * p[0] + t0[1] * p[1] + t0[2] * p[2]) * inv;
        if (u < 0.0f || u > 1.0f)
            continue;
        const float q[3] = {t0[1] * e1[2] - t0[2] * e1[1], t0[2] * e1[0] - t0[0] * e1[2], t0[0] * e1[1] - t0[1] * e1[0]};
        const float v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv;
        if (v < 0.0f || u + v > 1.0f)
            continue;
        const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (t >= 0.0f && t < best)
            best = t;
    }
    return best;
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
    // Aim view and facing: blend the rig as soon as R1 is pressed (with the
    // ready animation), and turn the character to the camera.
    const bool aim = aiming(ram) || aimHeld.load(std::memory_order_relaxed);
    static float aimSeconds = 0.0f;
    aimSeconds = aim ? aimSeconds + dt : 0.0f;
    aimPitch.store(pitch * 57.2957795f, std::memory_order_relaxed);
    // RE2-style reticle: shown while aiming, closing in over about a second.
    const float spread = std::fmax(0.15f, 1.0f - aimSeconds / 0.9f);
    reo::overlay::crosshair.store(aiming(ram) ? 1u | (uint32_t(std::lround(spread * 255.0f)) << 8) : 0u,
                                  std::memory_order_relaxed);
    if (dt > 0.0f)
        aimBlend = std::fmin(std::fmax(aimBlend + (aim ? dt : -dt) / kAimBlendSeconds, 0.0f), 1.0f);
    else
        aimBlend = aim ? 1.0f : 0.0f;
    const float t = aimBlend * aimBlend * (3.0f - 2.0f * aimBlend);
    auto mix = [t](float a, float b) { return a + (b - a) * t; };
    Rig view = rig;
    view.distance = mix(rig.distance, aimRig.distance);
    view.height = mix(rig.height, aimRig.height);
    view.side = mix(rig.side, aimRig.side);
    view.lookAhead = mix(rig.lookAhead, aimRig.lookAhead);
    view.fovDegrees = mix(rig.fovDegrees, aimRig.fovDegrees);
    if (aim) {
        const int16_t facing = int16_t(std::lround(yaw * 65536.0f / tau));
        std::memcpy(ram + actor + 0x92u, &facing, 2);
    } else if (freeGameplay(ram) && ram[actor + 8u] == 1u) {
        // Moving: face camera + stick direction (stick up = camera forward,
        // stick right = lower yaw), turning at a bounded rate.
        const float mx = moveX.load(std::memory_order_relaxed), my = moveY.load(std::memory_order_relaxed);
        if (mx != 0.0f || my != 0.0f) {
            const float desired = yaw - std::atan2(mx, -my);
            const float step = kTurnDegPerSec * degree * dt;
            const float delta = std::remainder(desired - heading, tau);
            const float turned = heading + std::fmin(std::fmax(delta, -step), step);
            const int16_t facing = int16_t(std::lround(std::remainder(turned, tau) * 65536.0f / tau));
            std::memcpy(ram + actor + 0x92u, &facing, 2);
        }
    }
    const float fx = std::sin(yaw), fz = std::cos(yaw);
    // Right of the facing direction (heading = atan2(x, z)).
    const float rx = -fz, rz = fx;
    // Orbit around the shoulder point; at the resting pitch this matches the rig.
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float dx = fx * cp, dy = -sp, dz = fz * cp;
    // Wall collision: cast from the character's chest toward the desired eye
    // against the room walls; pull the camera in front of the first hit
    // (immediately) and ease back out when the way is clear again.
    float& clearance = cameraClearance;
    float clear = 1.0f;
    if (collisionEnabled() && freeGameplay(ram)) {
        loadWalls(ram);
        const float chest[3] = {x, y + 120.0f, z};
        const float eyeFull[3] = {x + rx * view.side - dx * view.distance, y + view.height - dy * view.distance -
                                  std::sin(restingPitch()) * view.distance, z + rz * view.side - dz * view.distance};
        const float sx = eyeFull[0] - chest[0], sy = eyeFull[1] - chest[1], sz = eyeFull[2] - chest[2];
        const float length = std::sqrt(sx * sx + sy * sy + sz * sz);
        // Five rays (centre and the four edges of the view near the eye) so a
        // grazing wall cannot enter the edge of the picture; keep 35 units off.
        float hit = 1.0f;
        if (length > 1.0f) {
            const float offsets[5][2] = {{0, 0}, {28, 0}, {-28, 0}, {0, 18}, {0, -18}};
            for (const auto& o : offsets) {
                const float to[3] = {eyeFull[0] + rx * o[0], eyeFull[1] + o[1], eyeFull[2] + rz * o[0]};
                hit = std::fmin(hit, firstWallHit(chest, to));
            }
            if (hit < 1.0f)
                clear = std::max(0.0f, hit - 35.0f / length);
        }
    }
    clearance = clear < clearance ? clear : std::fmin(clear, clearance + dt * 1.5f);
    // Closest view: just behind the shoulder; the look direction is unchanged.
    const float closest = 22.0f, distance = closest + (view.distance - closest) * clearance;
    const float side = view.side * (0.55f + 0.45f * clearance);
    const float px = x + rx * side, py = y + view.height, pz = z + rz * side;
    const float ex = px - dx * distance, ey = py - dy * distance - std::sin(restingPitch()) * distance,
                ez = pz - dz * distance;
    const float reach = distance + view.lookAhead;
    wf32(ram, game.eye + 0x0u, ex);
    wf32(ram, game.eye + 0x4u, ey);
    wf32(ram, game.eye + 0x8u, ez);
    wf32(ram, game.eye + 0xcu, ex + dx * reach);
    wf32(ram, game.eye + 0x10u, ey + dy * reach);
    wf32(ram, game.eye + 0x14u, ez + dz * reach);
    applyFov(ram, view.fovDegrees);
    aimRay = {true, {ex, ey, ez}, {dx, dy, dz}};
    if (freeAimEnabled() && aiming(ram)) {
        // Face and aim at the target, or at the point the crosshair covers.
        const uint32_t target = maintainTarget(ram, actor);
        float point[3] = {ex + dx * 1200.0f, ey + dy * 1200.0f, ez + dz * 1200.0f};
        if (target)
            enemyPoint(ram, target, point);
        const float gx = point[0] - x, gz = point[2] - z, gy = (y + 130.0f) - point[1];
        const float horizontal = std::sqrt(gx * gx + gz * gz);
        if (horizontal > 30.0f) {
            const int16_t facing = int16_t(std::lround(std::atan2(gx, gz) * 65536.0f / tau));
            std::memcpy(ram + actor + 0x92u, &facing, 2);
            freeAimPitch = int16_t(std::clamp(std::lround(std::atan2(gy, horizontal) * 65536.0f / tau), -0x2000L, 0x2000L));
        }
        if (target)
            reo::overlay::crosshair.fetch_or(2u, std::memory_order_relaxed);
    }
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

inline bool freeAimActor(const uint8_t* ram, uint32_t actor) {
    return enabled && freeAimEnabled() && ram[game.localSlot] < 4u && actor == localActor(ram);
}
// o2 0x65ED70(actor a0, range f12) -> enemy (v0): aim target search on R1 / L1.
inline void targetSearch(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t actor = GPR_U32(ctx, 4);
    o2_0065ED70_0x65ed70(rdram, ctx, runtime);
    // +0xFF0 marks the online path that takes a target chosen elsewhere.
    if (!freeAimActor(rdram, actor) || rdram[actor + 0xff0u])
        return;
    const uint32_t enemy = pickTarget(rdram, actor, kLockConeDeg);
    setTarget(rdram, actor, enemy);
    SET_GPR_U32(ctx, 2, enemy);
}
// o2 0x586AA0(actor a0, ...): vertical aim; without a target it follows the crosshair.
inline void verticalAim(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t actor = GPR_U32(ctx, 4);
    o2_00586AA0_0x586aa0(rdram, ctx, runtime);
    if (freeAimActor(rdram, actor) && rdram[actor + 0xcdcu] == kNoTarget && aiming(rdram))
        std::memcpy(rdram + actor + 0xbc8u, &freeAimPitch, 2);
}

// REO_MODERN_CAMERA_PROBE=3: every 40 frame services outside free gameplay, write
// all 32 MiB of guest RAM to modcam_ram_<n>.bin and log the elapsed time
// (menu reverse engineering; RAM dumps stay in the private run folder).
inline void snapshotRam(const uint8_t* ram) {
    // Level 4: any game state, from REO_MODERN_CAMERA_SNAP_AFTER seconds (frontend menus).
    static const int level = [] {
        const char* value = std::getenv("REO_MODERN_CAMERA_PROBE");
        return value ? std::atoi(value) : 0;
    }();
    static const double after = [] {
        const char* value = std::getenv("REO_MODERN_CAMERA_SNAP_AFTER");
        return value ? std::atof(value) : 0.0;
    }();
    static unsigned calls = 0, index = 0;
    static const auto start = std::chrono::steady_clock::now();
    // Level 5: in gameplay, every 2nd frame-service call while aiming and every
    // 20th otherwise (aim-target reverse engineering).
    if (level == 5) {
        if (ram[game.gameState] != 2u || ++calls % (aiming(ram) ? 2u : 20u) != 0 || index >= 30)
            return;
    } else if (level != 3 && level != 4) {
        return;
    }
    if (level == 3 && (ram[game.gameState] != 2u || freeGameplay(ram)))
        return;
    if ((level == 4 || level == 5) && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < after)
        return;
    if (level != 5 && (++calls % 40 != 0 || index >= 30))
        return;
    char name[64];
    std::snprintf(name, sizeof(name), "modcam_ram_%03u.bin", index);
    if (std::FILE* out = std::fopen(name, "wb")) {
        std::fwrite(ram, 1, 0x2000000u, out);
        std::fclose(out);
        std::fprintf(stderr, "[MODCAM-SNAP] %u t=%.3f\n", index,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        ++index;
    }
}

// REO_MODERN_CAMERA_WATCH="0xADDR:len" prints that guest RAM window every 6
// frame services while it changes (menu reverse engineering).
inline void watchRam(const uint8_t* ram) {
    static uint32_t address = 0, length = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* text = std::getenv("REO_MODERN_CAMERA_WATCH"))
            std::sscanf(text, "%x:%x", &address, &length);
        if (length > 128u || address + length > 0x2000000u)
            length = 0;
    }
    static unsigned calls = 0;
    static uint8_t last[128] = {};
    static const auto start = std::chrono::steady_clock::now();
    if (!length || ++calls % 6 != 0 || std::memcmp(last, ram + address, length) == 0)
        return;
    std::memcpy(last, ram + address, length);
    std::fprintf(stderr, "[MODCAM-WATCH] t=%.3f", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    for (uint32_t i = 0; i < length; ++i)
        std::fprintf(stderr, " %02x", ram[address + i]);
    std::fprintf(stderr, "\n");
}

inline void service(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const bool entry = ctx->pc == game.frameService;
    if (entry && probe)
        watchRam(ram);
    if (entry && probe)
        probeFrame(ram);
    if (entry && probe)
        snapshotRam(ram);
    originalService(ram, ctx, runtime);
}

inline void install(PS2Runtime& runtime, Addresses addresses = File1) {
    if (const char* level = std::getenv("REO_MODERN_CAMERA_PROBE"))
        probe = std::strcmp(level, "1") == 0 || std::strcmp(level, "2") == 0 || std::strcmp(level, "3") == 0 ||
                std::strcmp(level, "4") == 0 || std::strcmp(level, "5") == 0;
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
    {
        runtime.registerOverlayFunction(game.overlayBase, 0x336f574du, 4u, game.overlayId, game.cameraApply,
                                        cameraApply);
        if (freeAimEnabled()) {
            // Gameplay overlay 2 (base 0x570000, id 3).
            runtime.registerOverlayFunction(0x570000u, 0x336f574du, 4u, 3u, 0x65ed70u, targetSearch);
            runtime.registerOverlayFunction(0x570000u, 0x336f574du, 4u, 3u, 0x586aa0u, verticalAim);
        }
    }
    std::fprintf(stderr,
                 "[MODCAM-1] installed follow=%d probe=%d rig=%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.2f "
                 "controls=%.0f,%.0f,%.2f,%.0f\n",
                 int(enabled), int(probe), rig.distance, rig.height, rig.side, rig.lookAhead, rig.lookHeight,
                 rig.fovDegrees, rig.recenter, controls.stickYaw, controls.stickPitch, controls.mouse,
                 controls.invertY);
}
} // namespace reo::modern_camera
