#pragma once

// Interface between native_hook_tests.exe and the unloadable native_hook_fixture.dll.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <cstdint>

using NativeFixtureCall = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

// Every object and synchronization handle belongs to the executable, not the unloadable DLL.
// The configuration is immutable between enable and a successful drain.
struct NativeFixtureState {
    // Hook original that each detour forwards to; the detour adds delta to its result.
    NativeFixtureCall original = nullptr;
    uintptr_t delta = 0;
    // While block is set, the detour signals entered and waits for release.
    HANDLE entered = nullptr;
    HANDLE release = nullptr;
    // Optional callback run inside the detour before forwarding.
    void (*action)(void*) = nullptr;
    void* action_context = nullptr;
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> wait_errors{0};
    std::atomic<bool> block{false};
};

// Exported as native_fixture_configure(index, state) for index 0 (detour_a) and 1 (detour_b).
using NativeFixtureConfigure = void (*)(unsigned, NativeFixtureState*);
