// Lifetime tests for ark::core::NativeHooks against a genuinely unloadable business DLL.
//
// Usage: native_hook_tests.exe <directory containing native_hook_fixture.dll>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <MinHook.h>

#include "native_hook_fixture.h"
#include "native_hooks.h"

namespace {

using ark::core::NativeHooks;

[[noreturn]] void fail(const char* expression, int line, const std::string& detail = {}) {
    std::fprintf(stderr, "FAIL native hooks line %d: %s %s\n", line, expression, detail.c_str());
    std::fflush(stderr);
    // A failed lifetime assertion must neither keep unwinding into an unsafe DLL nor block
    // forever joining a callback that exposed a teardown deadlock.
    ExitProcess(1);
}

} // namespace

#define ARK_CHECK(condition)                                                                       \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fail(#condition, __LINE__);                                                            \
        }                                                                                          \
    } while (0)

// Like ARK_CHECK, but reports the NativeHooks error string on failure.
#define ARK_CHECK_HOOK(condition, error)                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fail(#condition, __LINE__, error);                                                     \
        }                                                                                          \
    } while (0)

namespace {

constexpr DWORD kWaitMs = 5000;
constexpr DWORD kShortTimeoutMs = 20;
constexpr DWORD kCallbackTimeoutMs = 3000;
constexpr ULONGLONG kImmediateRejectMs = 1000;
constexpr DWORD kWatchdogMs = 120000;
// Each hook generation adds a distinct multiple of this to the native result.
constexpr uintptr_t kGenerationStep = uintptr_t{1} << 40;
constexpr uintptr_t kPointerValue = UINT64_C(0xd13579bdf2468ace);
constexpr wchar_t kFixtureDll[] = L"native_hook_fixture.dll";
constexpr wchar_t kPeerDll[] = L"native_hook_peer.dll";

// The unhooked target's result. b must point to a readable uintptr_t, so a corrupted second
// argument faults instead of passing silently.
uintptr_t expected(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    return ((a * 17) ^ *reinterpret_cast<const uintptr_t*>(b)) + c * 31 - d;
}

uintptr_t address_of(const uintptr_t& value) {
    return reinterpret_cast<uintptr_t>(&value);
}

// Manual-reset event owned by the executable.
class Event {
public:
    Event() { ARK_CHECK(handle_); }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    ~Event() { CloseHandle(handle_); }

    HANDLE handle() const { return handle_; }

    void set() const { ARK_CHECK(SetEvent(handle_)); }

    void wait() const { ARK_CHECK(WaitForSingleObject(handle_, kWaitMs) == WAIT_OBJECT_0); }

private:
    HANDLE handle_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
};

// An independently blockable, process-owned callee. It lets the retained-trampoline test
// return from a relocated CALL only after the business DLL is unmapped.
std::atomic<bool> native_callee_block{false};
HANDLE native_callee_entered = nullptr;
HANDLE native_callee_release = nullptr;
std::atomic<uint32_t> native_callee_wait_errors{0};

__declspec(noinline) uintptr_t native_callee(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    if (native_callee_block.load(std::memory_order_acquire)) {
        SetEvent(native_callee_entered);
        if (WaitForSingleObject(native_callee_release, kWaitMs) != WAIT_OBJECT_0) {
            native_callee_wait_errors.fetch_add(1);
        }
    }
    return ((a * 17) ^ *reinterpret_cast<const uintptr_t*>(b)) + c * 31 - d;
}

// A process-resident hook target generated at runtime:
//   +0   sub rsp,40; call +64; add rsp,40; ret
//   +64  mov rax,native_callee; jmp rax
//   +128 UNWIND_INFO for the function at +0
// MinHook must relocate both the stack adjustment and the relative CALL to cover its 5-byte
// patch.
class HookTarget {
public:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kLeafOffset = 64;
    static constexpr size_t kUnwindOffset = 128;
    static constexpr size_t kAfterCallOffset = 9;

    HookTarget() {
        code_ = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        ARK_CHECK(code_);
        std::memset(code_, 0xcc, kPageSize);
        const unsigned char body[] = {0x48, 0x83, 0xec, 0x28, 0xe8, 55,   0,
                                      0,    0,    0x48, 0x83, 0xc4, 0x28, 0xc3};
        std::memcpy(code_, body, sizeof body);
        code_[kLeafOffset] = 0x48;
        code_[kLeafOffset + 1] = 0xb8;
        auto destination = reinterpret_cast<void*>(native_callee);
        std::memcpy(code_ + kLeafOffset + 2, &destination, sizeof destination);
        code_[kLeafOffset + 10] = 0xff;
        code_[kLeafOffset + 11] = 0xe0;
        const unsigned char unwind[] = {1, 4, 1, 0, 4, 0x42, 0, 0};
        std::memcpy(code_ + kUnwindOffset, unwind, sizeof unwind);
        DWORD previous_protection = 0;
        ARK_CHECK(VirtualProtect(code_, kPageSize, PAGE_EXECUTE_READ, &previous_protection));
        ARK_CHECK(FlushInstructionCache(GetCurrentProcess(), code_, kPageSize));
        // Targets, leaf jump stubs and unwind records deliberately outlive all retired gates,
        // so both the code page and this function table entry are leaked. No business
        // allocation is retained by this fixture.
        auto* function = new RUNTIME_FUNCTION{0, sizeof body, kUnwindOffset};
        ARK_CHECK(RtlAddFunctionTable(function, 1, reinterpret_cast<DWORD64>(code_)));
    }

    unsigned char* code() const { return code_; }

    NativeFixtureCall call() const { return reinterpret_cast<NativeFixtureCall>(code_); }

    uintptr_t operator()(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) const {
        return call()(a, b, c, d);
    }

private:
    unsigned char* code_ = nullptr;
};

// Verifies a core gate is RX (never writable executable memory) and that its unwind data
// restores the caller's frame from the PC just before the gate's CALL.
void check_gate(void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    ARK_CHECK(VirtualQuery(address, &memory, sizeof memory) == sizeof memory);
    ARK_CHECK(memory.State == MEM_COMMIT);
    ARK_CHECK((memory.Protect & 0xff) == PAGE_EXECUTE_READ);
    DWORD64 image = 0;
    constexpr DWORD64 kOffsetBeforeCall = 38;
    const auto pc = reinterpret_cast<DWORD64>(address) + kOffsetBeforeCall;
    auto* function = RtlLookupFunctionEntry(pc, &image, nullptr);
    ARK_CHECK(function);
    alignas(16) DWORD64 stack[16]{};
    constexpr DWORD64 kReturnPc = UINT64_C(0x123456789abcdef0);
    constexpr DWORD64 kPreservedRbx = UINT64_C(0xaabbccdd11223344);
    // The gate's 40-byte frame is followed by the caller's return PC.
    stack[5] = kReturnPc;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    context.Rip = pc;
    context.Rsp = reinterpret_cast<DWORD64>(stack);
    context.Rbx = kPreservedRbx;
    void* handler = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, function, &context, &handler, &frame, nullptr);
    ARK_CHECK(context.Rip == kReturnPc);
    ARK_CHECK(context.Rsp == reinterpret_cast<DWORD64>(stack + 6));
    ARK_CHECK(context.Rbx == kPreservedRbx);
}

struct PatchedEntries {
    // The core anchor gate MinHook jumps to.
    NativeFixtureCall gate;
    // MinHook's retained trampoline for the target.
    NativeFixtureCall trampoline;
};

// Decodes the installed MinHook x64 patch and relay rather than adding a testing accessor to
// the production lifetime API. The layout is checked against the vendored emitter before the
// trampoline address is derived.
PatchedEntries decode_patch(const HookTarget& target) {
    const auto* code = target.code();
    ARK_CHECK(code[0] == 0xe9);
    int32_t relative = 0;
    std::memcpy(&relative, code + 1, sizeof relative);
    auto* relay = reinterpret_cast<unsigned char*>(reinterpret_cast<intptr_t>(code + 5) + relative);
    const unsigned char indirect_jump[] = {0xff, 0x25, 0, 0, 0, 0};
    ARK_CHECK(std::memcmp(relay, indirect_jump, sizeof indirect_jump) == 0);
    void* gate = nullptr;
    std::memcpy(&gate, relay + 6, sizeof gate);
    // This exact target emits a 4-byte SUB, a 16-byte CALL_ABS and a 14-byte JMP_ABS.
    auto* trampoline = relay - 34;
    const unsigned char sub[] = {0x48, 0x83, 0xec, 0x28};
    const unsigned char call[] = {0xff, 0x15, 2, 0, 0, 0, 0xeb, 8};
    ARK_CHECK(std::memcmp(trampoline, sub, sizeof sub) == 0);
    ARK_CHECK(std::memcmp(trampoline + 4, call, sizeof call) == 0);
    void* called = nullptr;
    std::memcpy(&called, trampoline + 12, sizeof called);
    ARK_CHECK(called == code + HookTarget::kLeafOffset);
    ARK_CHECK(std::memcmp(trampoline + 20, indirect_jump, sizeof indirect_jump) == 0);
    void* continued = nullptr;
    std::memcpy(&continued, trampoline + 26, sizeof continued);
    ARK_CHECK(continued == code + HookTarget::kAfterCallOffset);
    return {reinterpret_cast<NativeFixtureCall>(gate),
            reinterpret_cast<NativeFixtureCall>(trampoline)};
}

// One loaded copy of the fixture DLL acting as a hook owner with up to two hooks.
struct FixtureModule {
    NativeHooks& hooks;
    HMODULE module = nullptr;
    std::filesystem::path path;
    NativeFixtureConfigure configure = nullptr;
    std::array<uint64_t, 2> ids{};
    std::array<NativeFixtureState, 2> states{};
    std::array<void*, 2> detours{};

    FixtureModule(NativeHooks& manager, const std::filesystem::path& directory,
                  const wchar_t* filename = kFixtureDll)
        : hooks(manager),
          path(directory / filename) {
        module = LoadLibraryW(path.c_str());
        ARK_CHECK(module);
        configure = reinterpret_cast<NativeFixtureConfigure>(
            GetProcAddress(module, "native_fixture_configure"));
        detours[0] = reinterpret_cast<void*>(GetProcAddress(module, "native_fixture_detour_a"));
        detours[1] = reinterpret_cast<void*>(GetProcAddress(module, "native_fixture_detour_b"));
        ARK_CHECK(configure && detours[0] && detours[1]);
    }

    void create(unsigned index, const HookTarget& target, uintptr_t delta) {
        std::string error;
        void* original = nullptr;
        ids[index] = hooks.create(this, target.code(), detours[index], &original, error);
        ARK_CHECK_HOOK(ids[index], error);
        states[index].original = reinterpret_cast<NativeFixtureCall>(original);
        states[index].delta = delta;
        configure(index, &states[index]);
    }

    void enable(unsigned index, bool enabled = true) {
        std::string error;
        ARK_CHECK_HOOK(hooks.enable(this, ids[index], enabled, error), error);
    }

    void remove(unsigned index) {
        std::string error;
        ARK_CHECK_HOOK(hooks.remove(this, ids[index], kWaitMs, error), error);
        ids[index] = 0;
        ARK_CHECK(states[index].active.load() == 0);
        ARK_CHECK(states[index].wait_errors.load() == 0);
    }

    // Frees the DLL and proves it is truly unmapped, not merely dereferenced.
    void unload() {
        ARK_CHECK(module && !ids[0] && !ids[1]);
        ARK_CHECK(!states[0].active.load() && !states[1].active.load());
        auto* old_module = module;
        auto* old_detour = detours[0];
        ARK_CHECK(FreeLibrary(module));
        module = nullptr;
        configure = nullptr;
        detours = {};
        ARK_CHECK(!GetModuleHandleW(path.c_str()));
        HMODULE found = nullptr;
        ARK_CHECK(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(old_detour), &found));
        MEMORY_BASIC_INFORMATION memory{};
        ARK_CHECK(VirtualQuery(old_detour, &memory, sizeof memory) == sizeof memory);
        ARK_CHECK(memory.State != MEM_COMMIT || memory.AllocationBase != old_module);
    }
};

// Starts target(a, b, c, d) on a new thread; the result is stored when the call returns.
std::thread call_on_thread(const HookTarget& target, uintptr_t& result, const Event& done,
                           uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    return std::thread([&target, &result, &done, a, b, c, d] {
        result = target(a, b, c, d);
        done.set();
    });
}

void block_detour(NativeFixtureState& state, const Event& entered, const Event& release) {
    state.entered = entered.handle();
    state.release = release.handle();
    state.block.store(true);
}

void test_forwarding(NativeHooks& hooks, const std::filesystem::path& directory) {
    HookTarget target;
    FixtureModule fixture(hooks, directory);
    fixture.create(0, target, kGenerationStep);
    auto original = fixture.states[0].original;
    check_gate(reinterpret_cast<void*>(original));

    const uintptr_t values[] = {kPointerValue, UINT64_C(0x87654321fedcba09)};
    for (const auto& value : values) {
        const auto b = address_of(value);
        const auto base = expected(UINT64_C(0xfedcba9876543210), b, UINT64_C(0x123456789abcdef0),
                                   UINT64_C(0x8899aabbccddeeff));
        ARK_CHECK(original(UINT64_C(0xfedcba9876543210), b, UINT64_C(0x123456789abcdef0),
                           UINT64_C(0x8899aabbccddeeff)) == base);
    }

    fixture.enable(0);
    const auto saved = decode_patch(target);
    check_gate(reinterpret_cast<void*>(saved.gate));
    for (unsigned i = 0; i != 32; ++i) {
        const auto a = UINT64_C(0xfedcba9876543210) + i;
        const auto b = address_of(values[i % 2]);
        const auto c = UINT64_C(0x123456789abcdef0) - i;
        const auto d = UINT64_C(0x8899aabbccddeeff) ^ i;
        const auto base = expected(a, b, c, d);
        ARK_CHECK(target(a, b, c, d) == base + kGenerationStep);
        ARK_CHECK(original(a, b, c, d) == base);
        ARK_CHECK(saved.gate(a, b, c, d) == base + kGenerationStep);
    }

    const auto b = address_of(values[0]);
    fixture.enable(0, false);
    ARK_CHECK(saved.gate(1, b, 3, 4) == expected(1, b, 3, 4));
    fixture.enable(0);
    ARK_CHECK(target(1, b, 3, 4) == expected(1, b, 3, 4) + kGenerationStep);
    fixture.remove(0);
    fixture.unload();
    ARK_CHECK(original(1, b, 3, 4) == expected(1, b, 3, 4));
    ARK_CHECK(saved.gate(1, b, 3, 4) == expected(1, b, 3, 4));
    ARK_CHECK(saved.trampoline(1, b, 3, 4) == expected(1, b, 3, 4));

    // The same target receives a genuinely new DLL generation. Retired entry points must
    // follow that target and never retain generation one's detour.
    FixtureModule next(hooks, directory);
    next.create(0, target, kGenerationStep * 2);
    next.enable(0);
    ARK_CHECK(original(1, b, 3, 4) == expected(1, b, 3, 4) + kGenerationStep * 2);
    ARK_CHECK(saved.gate(1, b, 3, 4) == expected(1, b, 3, 4) + kGenerationStep * 2);
    next.remove(0);
    next.unload();
    ARK_CHECK(original(1, b, 3, 4) == expected(1, b, 3, 4));
    std::puts("PASS native argument/return forwarding, RX/unwind gates, disable/enable, true DLL "
              "unmap, retired replay and same-target rehook");
}

void test_shared_target(NativeHooks& hooks, const std::filesystem::path& directory) {
    const auto peer_path = directory / kPeerDll;
    std::filesystem::copy_file(directory / kFixtureDll, peer_path,
                               std::filesystem::copy_options::overwrite_existing);
    HookTarget target;
    FixtureModule older(hooks, directory);
    FixtureModule newer(hooks, directory, kPeerDll);
    older.create(0, target, kGenerationStep);
    older.enable(0);
    newer.create(0, target, kGenerationStep * 2);
    newer.enable(0);
    const auto old_original = older.states[0].original;
    const auto peer_original = newer.states[0].original;
    const auto saved = decode_patch(target);
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);
    const auto base = expected(5, b, 7, 8);

    // Ordered originals: each continues below its own link only.
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 3);
    ARK_CHECK(old_original(5, b, 7, 8) == base);
    ARK_CHECK(peer_original(5, b, 7, 8) == base + kGenerationStep);

    // Isolated enable/disable per owner.
    older.enable(0, false);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 2);
    ARK_CHECK(peer_original(5, b, 7, 8) == base);
    newer.enable(0, false);
    ARK_CHECK(target(5, b, 7, 8) == base);
    older.enable(0);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep);
    newer.enable(0);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 3);

    // Removing the older owner while its detour is blocked times out, but already seals it.
    {
        Event entered;
        Event release;
        Event done;
        auto& state = older.states[0];
        block_detour(state, entered, release);
        uintptr_t result = 0;
        auto call = call_on_thread(target, result, done, 5, b, 7, 8);
        entered.wait();
        std::string error;
        ARK_CHECK(!hooks.remove(&older, older.ids[0], kShortTimeoutMs, error));
        ARK_CHECK(!error.empty() && state.active.load() == 1 && newer.states[0].active.load() == 1);
        ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 2);
        // A retired external original follows the live head. Its lower-chain occurrence must
        // skip itself instead of returning to that same head.
        ARK_CHECK(old_original(5, b, 7, 8) == base + kGenerationStep * 2);
        ARK_CHECK(peer_original(5, b, 7, 8) == base);
        release.set();
        done.wait();
        call.join();
        ARK_CHECK(result == base + kGenerationStep * 3);
        older.remove(0);
        older.unload();
    }
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 2);
    ARK_CHECK(old_original(5, b, 7, 8) == base + kGenerationStep * 2);

    FixtureModule replacement(hooks, directory);
    replacement.create(0, target, kGenerationStep * 4);
    replacement.enable(0);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 6);
    ARK_CHECK(replacement.states[0].original(5, b, 7, 8) == base + kGenerationStep * 2);
    // An older original never visits the new head.
    ARK_CHECK(peer_original(5, b, 7, 8) == base);

    // Detaching the newer owner while its detour is blocked times out, but already seals it.
    {
        Event entered;
        Event release;
        Event done;
        block_detour(newer.states[0], entered, release);
        uintptr_t result = 0;
        auto call = call_on_thread(target, result, done, 5, b, 7, 8);
        entered.wait();
        std::string error;
        ARK_CHECK(!hooks.detach(&newer, kShortTimeoutMs, error));
        ARK_CHECK(!error.empty());
        ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 4);
        ARK_CHECK(peer_original(5, b, 7, 8) == base + kGenerationStep * 4);
        release.set();
        done.wait();
        call.join();
        ARK_CHECK(result == base + kGenerationStep * 6);
        newer.remove(0);
        newer.unload();
    }
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 4);
    ARK_CHECK(saved.gate(5, b, 7, 8) == base + kGenerationStep * 4);
    ARK_CHECK(old_original(5, b, 7, 8) == base + kGenerationStep * 4);
    ARK_CHECK(peer_original(5, b, 7, 8) == base + kGenerationStep * 4);

    // Reloading the peer DLL on the same target.
    FixtureModule peer_reload(hooks, directory, kPeerDll);
    peer_reload.create(0, target, kGenerationStep * 8);
    peer_reload.enable(0);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 12);
    ARK_CHECK(peer_reload.states[0].original(5, b, 7, 8) == base + kGenerationStep * 4);
    peer_reload.enable(0, false);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 4);
    peer_reload.enable(0);
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 12);
    peer_reload.remove(0);
    peer_reload.unload();
    ARK_CHECK(target(5, b, 7, 8) == base + kGenerationStep * 4);
    replacement.remove(0);
    replacement.unload();
    ARK_CHECK(target(5, b, 7, 8) == base);
    ARK_CHECK(old_original(5, b, 7, 8) == base && peer_original(5, b, 7, 8) == base);
    std::filesystem::remove(peer_path);
    std::puts("PASS two independently unmapped owners share a target: ordered originals, isolated "
              "disable/remove/reload, concurrent drain and late replay");
}

void test_blocked_timeout(NativeHooks& hooks, const std::filesystem::path& directory) {
    HookTarget target;
    FixtureModule fixture(hooks, directory);
    Event entered;
    Event release;
    Event done;
    fixture.create(0, target, kGenerationStep);
    auto& state = fixture.states[0];
    block_detour(state, entered, release);
    fixture.enable(0);
    const auto saved = decode_patch(target);
    const auto original = state.original;
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);
    uintptr_t result = 0;
    auto callback = call_on_thread(target, result, done, 11, b, 33, 44);
    entered.wait();

    std::string error;
    const auto started = GetTickCount64();
    ARK_CHECK(!hooks.remove(&fixture, fixture.ids[0], kShortTimeoutMs, error));
    ARK_CHECK(!error.empty());
    ARK_CHECK(GetTickCount64() - started < kImmediateRejectMs);
    ARK_CHECK(GetModuleHandleW(fixture.path.c_str()) == fixture.module);
    ARK_CHECK(state.active.load() == 1);
    ARK_CHECK(!hooks.enable(&fixture, fixture.ids[0], true, error));
    // Sealed gates may be replayed while the timed-out generation still runs; they must not
    // enter that business callback again or share its blockade.
    ARK_CHECK(saved.gate(11, b, 33, 44) == expected(11, b, 33, 44));
    ARK_CHECK(original(11, b, 33, 44) == expected(11, b, 33, 44));
    ARK_CHECK(state.calls.load() == 1);

    release.set();
    done.wait();
    callback.join();
    ARK_CHECK(result == expected(11, b, 33, 44) + kGenerationStep);
    fixture.remove(0);
    fixture.unload();
    ARK_CHECK(saved.gate(11, b, 33, 44) == expected(11, b, 33, 44));
    ARK_CHECK(original(11, b, 33, 44) == expected(11, b, 33, 44));
    std::puts("PASS blocked callback timeout retains live DLL/state; sealed replay and removal "
              "retry");
}

struct RemovalAttempt {
    NativeHooks* hooks;
    FixtureModule* fixture;
    NativeFixtureCall nested = nullptr;
    std::atomic<unsigned> attempts{0};
};

// Fixture action: removing or detaching hook 0's owner from inside a callback must fail fast.
void attempt_removal(void* context) {
    auto& attempt = *static_cast<RemovalAttempt*>(context);
    std::string error;
    const auto started = GetTickCount64();
    ARK_CHECK(!attempt.hooks->remove(attempt.fixture, attempt.fixture->ids[0], kCallbackTimeoutMs,
                                     error));
    ARK_CHECK(!error.empty());
    ARK_CHECK(!attempt.hooks->detach(attempt.fixture, kCallbackTimeoutMs, error));
    ARK_CHECK(!error.empty());
    ARK_CHECK(GetTickCount64() - started < kImmediateRejectMs);
    attempt.attempts.fetch_add(1);
}

// Fixture action: calls the inner hooked target from inside the outer callback.
void call_nested_target(void* context) {
    auto& attempt = *static_cast<RemovalAttempt*>(context);
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);
    ARK_CHECK(attempt.nested(7, b, 9, 10) == expected(7, b, 9, 10) + kGenerationStep * 2);
}

void test_callback_removal(NativeHooks& hooks, const std::filesystem::path& directory) {
    HookTarget outer;
    HookTarget inner;
    FixtureModule fixture(hooks, directory);
    fixture.create(0, outer, kGenerationStep);
    fixture.create(1, inner, kGenerationStep * 2);
    RemovalAttempt attempt{&hooks, &fixture, inner.call()};
    fixture.states[0].action = attempt_removal;
    fixture.states[0].action_context = &attempt;
    fixture.enable(0);
    fixture.enable(1);
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);

    // Self-removal from hook 0's own callback.
    ARK_CHECK(outer(1, b, 3, 4) == expected(1, b, 3, 4) + kGenerationStep);
    ARK_CHECK(attempt.attempts.load() == 1);

    // Ancestor removal from the nested hook 1 callback. No callbacks are running during this
    // configuration change.
    fixture.states[0].action = call_nested_target;
    fixture.states[1].action = attempt_removal;
    fixture.states[1].action_context = &attempt;
    ARK_CHECK(outer(1, b, 3, 4) == expected(1, b, 3, 4) + kGenerationStep);
    ARK_CHECK(attempt.attempts.load() == 2);
    ARK_CHECK(fixture.states[0].calls.load() == 2 && fixture.states[1].calls.load() == 1);

    fixture.remove(0);
    fixture.remove(1);
    fixture.unload();
    std::puts("PASS self/nested ancestor removal and owner detach reject immediately without "
              "disabling live hooks");
}

struct CrossRemoval {
    NativeHooks* hooks;
    FixtureModule* fixture;
    uint64_t other_id;
    std::atomic<unsigned>* entered;
    std::atomic<unsigned>* attempted;
    Event* both_entered;
    Event* both_attempted;
};

// Fixture action: once both callbacks run, each tries to remove the other's hook.
void remove_other_hook(void* context) {
    auto& removal = *static_cast<CrossRemoval*>(context);
    if (removal.entered->fetch_add(1) + 1 == 2) {
        removal.both_entered->set();
    }
    removal.both_entered->wait();
    std::string error;
    const auto started = GetTickCount64();
    ARK_CHECK(!removal.hooks->remove(removal.fixture, removal.other_id, kCallbackTimeoutMs, error));
    ARK_CHECK(!error.empty());
    ARK_CHECK(GetTickCount64() - started < kImmediateRejectMs);
    if (removal.attempted->fetch_add(1) + 1 == 2) {
        removal.both_attempted->set();
    }
    // Keep each callback admitted until BOTH removers have returned. Otherwise the second
    // removal might legitimately find an already drained callback.
    removal.both_attempted->wait();
}

void test_cross_callback_removal(NativeHooks& hooks, const std::filesystem::path& directory) {
    HookTarget first;
    HookTarget second;
    FixtureModule fixture(hooks, directory);
    fixture.create(0, first, kGenerationStep);
    fixture.create(1, second, kGenerationStep * 2);
    Event both_entered;
    Event both_attempted;
    Event first_done;
    Event second_done;
    std::atomic<unsigned> entered{0};
    std::atomic<unsigned> attempted{0};
    CrossRemoval removals[] = {
        {&hooks, &fixture, fixture.ids[1], &entered, &attempted, &both_entered, &both_attempted},
        {&hooks, &fixture, fixture.ids[0], &entered, &attempted, &both_entered, &both_attempted},
    };
    for (unsigned index = 0; index != 2; ++index) {
        fixture.states[index].action = remove_other_hook;
        fixture.states[index].action_context = &removals[index];
        fixture.enable(index);
    }
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);
    uintptr_t first_result = 0;
    uintptr_t second_result = 0;
    auto first_call = call_on_thread(first, first_result, first_done, 1, b, 3, 4);
    auto second_call = call_on_thread(second, second_result, second_done, 5, b, 7, 8);
    first_done.wait();
    second_done.wait();
    first_call.join();
    second_call.join();
    ARK_CHECK(attempted.load() == 2);
    ARK_CHECK(first_result == expected(1, b, 3, 4) + kGenerationStep);
    ARK_CHECK(second_result == expected(5, b, 7, 8) + kGenerationStep * 2);
    fixture.remove(0);
    fixture.remove(1);
    fixture.unload();
    std::puts("PASS reciprocal cross-thread callback removals refuse cyclic drain waits; "
              "outside-callback retries succeed");
}

void test_concurrent_generations(NativeHooks& hooks, const std::filesystem::path& directory) {
    constexpr unsigned kGenerations = 24;
    constexpr unsigned kWorkerCount = 4;
    constexpr uint32_t kMinCallsPerGeneration = 32;
    HookTarget target;
    std::array<NativeFixtureCall, kGenerations> originals{};
    std::array<NativeFixtureCall, kGenerations> gates{};
    std::atomic<unsigned> published{0};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> completed{0};
    std::array<Event, kWorkerCount> done;
    std::vector<std::thread> workers;
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);

    // Workers mix direct target calls with replays of every published original and gate.
    for (unsigned worker = 0; worker != kWorkerCount; ++worker) {
        workers.emplace_back([&, worker] {
            unsigned cursor = worker;
            while (!stop.load(std::memory_order_acquire)) {
                auto call = target.call();
                const auto count = published.load(std::memory_order_acquire);
                if (count && cursor % 3) {
                    call = cursor % 2 ? originals[cursor % count] : gates[cursor % count];
                }
                const uintptr_t a = UINT64_C(0xfedcba9876543210) + cursor;
                const uintptr_t c = 0x99887766 + worker;
                const uintptr_t d = 0x11223344 + cursor;
                const auto difference = call(a, b, c, d) - expected(a, b, c, d);
                ARK_CHECK(difference % kGenerationStep == 0);
                ARK_CHECK(difference / kGenerationStep <= kGenerations);
                ++cursor;
                completed.fetch_add(1, std::memory_order_release);
            }
            done[worker].set();
        });
    }

    for (unsigned generation = 0; generation != kGenerations; ++generation) {
        FixtureModule fixture(hooks, directory);
        fixture.create(0, target, kGenerationStep * (generation + 1));
        fixture.enable(0);
        const auto saved = decode_patch(target);
        originals[generation] = fixture.states[0].original;
        gates[generation] = saved.gate;
        published.store(generation + 1, std::memory_order_release);
        // Require actual detour traffic in every generation, not merely threads that happen
        // to run only between load/unload operations.
        const auto deadline = GetTickCount64() + kWaitMs;
        while (fixture.states[0].calls.load(std::memory_order_acquire) < kMinCallsPerGeneration &&
               GetTickCount64() < deadline) {
            Sleep(1);
        }
        ARK_CHECK(fixture.states[0].calls.load() >= kMinCallsPerGeneration);
        ARK_CHECK(originals[generation](5, b, 7, 8) == expected(5, b, 7, 8));
        fixture.remove(0);
        fixture.unload();
        for (unsigned retired = 0; retired <= generation; ++retired) {
            ARK_CHECK(originals[retired](5, b, 7, 8) == expected(5, b, 7, 8));
            ARK_CHECK(gates[retired](5, b, 7, 8) == expected(5, b, 7, 8));
        }
    }

    stop.store(true, std::memory_order_release);
    for (unsigned worker = 0; worker != kWorkerCount; ++worker) {
        done[worker].wait();
        workers[worker].join();
    }
    ARK_CHECK(completed.load() >= kGenerations * kMinCallsPerGeneration);
    std::printf("PASS %u truly unloaded generations under %u concurrent native/retired-gate "
                "callers (%llu calls)\n",
                kGenerations, kWorkerCount, static_cast<unsigned long long>(completed.load()));
}

void test_relocated_call(NativeHooks& hooks, const std::filesystem::path& directory) {
    HookTarget target;
    FixtureModule fixture(hooks, directory);
    fixture.create(0, target, kGenerationStep);
    fixture.enable(0);
    const auto saved = decode_patch(target);
    Event entered;
    Event release;
    Event done;
    native_callee_entered = entered.handle();
    native_callee_release = release.handle();
    native_callee_wait_errors.store(0);
    native_callee_block.store(true, std::memory_order_release);
    const uintptr_t value = kPointerValue;
    const auto b = address_of(value);
    uintptr_t result = 0;
    // Direct raw-trampoline admission deterministically models a native thread MinHook
    // redirected at the target prologue, bypassing both counted gates. It is deliberately
    // neither a fixture export nor a business-owned trampoline.
    std::thread native([&] {
        result = saved.trampoline(13, b, 17, 19);
        done.set();
    });
    entered.wait();
    ARK_CHECK(fixture.states[0].calls.load() == 0);
    fixture.remove(0);
    fixture.unload();
    ARK_CHECK(WaitForSingleObject(done.handle(), 0) == WAIT_TIMEOUT);
    // The callee still has a return PC inside the relocated CALL sequence when removal and
    // unmap finish. Reusing or freeing that trampoline would fault here.
    native_callee_block.store(false, std::memory_order_release);
    release.set();
    done.wait();
    native.join();
    ARK_CHECK(native_callee_wait_errors.load() == 0);
    ARK_CHECK(result == expected(13, b, 17, 19));
    ARK_CHECK(saved.trampoline(13, b, 17, 19) == expected(13, b, 17, 19));
    ARK_CHECK(saved.gate(13, b, 17, 19) == expected(13, b, 17, 19));
    native_callee_entered = nullptr;
    native_callee_release = nullptr;
    std::puts("PASS relocated-CALL return survives successful removal and business DLL unmap "
              "outside counted gates");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ARK_CHECK(argc == 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    ARK_CHECK(std::filesystem::is_regular_file(directory / kFixtureDll));
    ARK_CHECK(MH_Initialize() == MH_OK);
    Event suite_done;
    std::thread watchdog([&] {
        if (WaitForSingleObject(suite_done.handle(), kWatchdogMs) != WAIT_OBJECT_0) {
            fail("suite watchdog expired (possible native teardown deadlock)", __LINE__);
        }
    });

    NativeHooks hooks;
    test_forwarding(hooks, directory);
    test_shared_target(hooks, directory);
    test_blocked_timeout(hooks, directory);
    test_callback_removal(hooks, directory);
    test_cross_callback_removal(hooks, directory);
    test_concurrent_generations(hooks, directory);
    test_relocated_call(hooks, directory);

    // Core gates and targets and the retained MinHook trampolines have process lifetime;
    // MH_Uninitialize would break that contract, so process exit owns them.
    suite_done.set();
    watchdog.join();
    std::puts("PASS native_hook_tests");
    return 0;
}
