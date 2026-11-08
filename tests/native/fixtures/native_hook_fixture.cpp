// Unloadable business DLL for native_hook_tests: two exported detours whose behavior is
// configured through executable-owned NativeFixtureState objects.

#include "native_hook_fixture.h"

namespace {

constexpr unsigned kDetourCount = 2;
constexpr DWORD kReleaseWaitMs = 5000;

NativeFixtureState* states[kDetourCount]{};

uintptr_t run_detour(unsigned index, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    auto& state = *states[index];
    state.active.fetch_add(1, std::memory_order_acq_rel);
    state.calls.fetch_add(1, std::memory_order_relaxed);
    if (state.block.load(std::memory_order_acquire)) {
        SetEvent(state.entered);
        if (WaitForSingleObject(state.release, kReleaseWaitMs) != WAIT_OBJECT_0) {
            state.wait_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (state.action) {
        state.action(state.action_context);
    }
    const auto result = state.original(a, b, c, d) + state.delta;
    state.active.fetch_sub(1, std::memory_order_release);
    return result;
}

} // namespace

extern "C" __declspec(dllexport) void native_fixture_configure(unsigned index,
                                                               NativeFixtureState* state) {
    if (index < kDetourCount) {
        states[index] = state;
    }
}

extern "C" __declspec(dllexport) uintptr_t native_fixture_detour_a(uintptr_t a, uintptr_t b,
                                                                   uintptr_t c, uintptr_t d) {
    return run_detour(0, a, b, c, d);
}

extern "C" __declspec(dllexport) uintptr_t native_fixture_detour_b(uintptr_t a, uintptr_t b,
                                                                   uintptr_t c, uintptr_t d) {
    return run_detour(1, a, b, c, d);
}
