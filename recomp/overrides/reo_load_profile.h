#pragma once
// Room-load profiler (diagnostics only, REO_LOAD_PROFILE=1): wraps the File #1
// room-setup calls made while entering a room and logs any call that takes more
// than 30 ms of wall time, to find what the loading screen waits for.
#include "ps2_runtime.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void o2_005EB6D0_0x5eb6d0(uint8_t*, R5900Context*, PS2Runtime*);
void o2_005EB780_0x5eb780(uint8_t*, R5900Context*, PS2Runtime*);
void o2_005EC3A0_0x5ec3a0(uint8_t*, R5900Context*, PS2Runtime*);
void o2_0068C510_0x68c510(uint8_t*, R5900Context*, PS2Runtime*);
void o2_005EBE30_0x5ebe30(uint8_t*, R5900Context*, PS2Runtime*);
void o2_005EC100_0x5ec100(uint8_t*, R5900Context*, PS2Runtime*);

namespace reo::load_profile {
using Function = void (*)(uint8_t*, R5900Context*, PS2Runtime*);
struct Entry {
    uint32_t address;
    bool overlay;
    Function original;
};
inline Entry entries[] = {
    {0x5eb6d0u, true, o2_005EB6D0_0x5eb6d0}, {0x5eb780u, true, o2_005EB780_0x5eb780},
    {0x5ec3a0u, true, o2_005EC3A0_0x5ec3a0}, {0x68c510u, true, o2_0068C510_0x68c510},
    {0x5ebe30u, true, o2_005EBE30_0x5ebe30}, {0x5ec100u, true, o2_005EC100_0x5ec100},
    {0x1ad750u, false, nullptr},             {0x1ac630u, false, nullptr},
    {0x1d1a80u, false, nullptr},             {0x1d41f0u, false, nullptr},
    {0x1aff60u, false, nullptr},             {0x1b2d00u, false, nullptr},
    {0x1b2e10u, false, nullptr},             {0x1b96d0u, false, nullptr},
    {0x1cfc00u, false, nullptr},             {0x1d0210u, false, nullptr},
    {0x1d3830u, false, nullptr},             {0x18dcb0u, false, nullptr},
    {0x1cd1c0u, false, nullptr},             {0x1ce8b0u, false, nullptr},
    {0x1ced20u, false, nullptr},             {0x1cf090u, false, nullptr},
    {0x1cf740u, false, nullptr},             {0x1cf9d0u, false, nullptr},
    {0x1d03a0u, false, nullptr},             {0x1aa4c0u, false, nullptr},
    {0x1b1820u, false, nullptr},             {0x1cd080u, false, nullptr},
    {0x1d0a70u, false, nullptr},             {0x1d0c80u, false, nullptr},
    {0x1d1f50u, false, nullptr},             {0x1d25a0u, false, nullptr},
};

template <int I>
void wrapper(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const auto start = std::chrono::steady_clock::now();
    entries[I].original(ram, ctx, runtime);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms > 15.0)
        std::fprintf(stderr, "[LOAD-PROFILE] %06x %.1f ms state=%u\n", entries[I].address, ms, ram[0x4badd0u]);
}

template <int I>
void install(PS2Runtime& runtime) {
    if constexpr (I < int(sizeof(entries) / sizeof(entries[0]))) {
        Entry& e = entries[I];
        if (e.overlay) {
            runtime.registerOverlayFunction(0x570000u, 0x336f574du, 4u, 3u, e.address, wrapper<I>);
        } else if ((e.original = runtime.lookupFunction(e.address))) {
            runtime.registerFunction(e.address, wrapper<I>);
        }
        install<I + 1>(runtime);
    }
}

inline void install(PS2Runtime& runtime) {
    const char* value = std::getenv("REO_LOAD_PROFILE");
    if (!value || std::strcmp(value, "1") != 0)
        return;
    install<0>(runtime);
    std::fprintf(stderr, "[LOAD-PROFILE] installed\n");
}
} // namespace reo::load_profile
