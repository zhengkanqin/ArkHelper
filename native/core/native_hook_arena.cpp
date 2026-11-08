// Process-lifetime arena of hook slots and their executable gates.
//
// Slots, generated code, unwind tables and MinHook's retained trampolines have process
// lifetime and are never reassigned to a different hook generation, so a late native entrant
// can always run the gate it captured. Chunked allocation keeps the hot path allocation-free
// and generated code W^X (written while RW, then sealed RX before publication).

#include <windows.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "native_hook_detail.h"

namespace ark::core {
namespace {

constexpr size_t kSlotsPerChunk = 256;
constexpr size_t kGateStride = 64;
constexpr size_t kGatesPerChunk = kSlotsPerChunk * 2;
constexpr size_t kGateCodeSize = kGateStride * kGatesPerChunk;
constexpr size_t kGateSize = 45;
constexpr size_t kUnwindInfoSize = 8;

// Byte offsets of the 64-bit immediates patched into each gate.
constexpr size_t kGateSlotImmediate = 20;
constexpr size_t kGateGatewayImmediate = 30;

// A gate turns gateway(slot, a, b, c, d) into a native four-argument function. On entry the
// caller's RSP is 8 mod 16: reserve 32 bytes of shadow space plus the fifth argument slot,
// shift RCX/RDX/R8/R9 one register to the right and load the slot into RCX.
constexpr std::array<unsigned char, kGateSize> kGateTemplate = {
    0x48, 0x83, 0xec, 0x28,                      // sub rsp,40
    0x4c, 0x89, 0x4c, 0x24, 0x20,                // mov [rsp+32],r9
    0x4d, 0x89, 0xc1,                            // mov r9,r8
    0x49, 0x89, 0xd0,                            // mov r8,rdx
    0x48, 0x89, 0xca,                            // mov rdx,rcx
    0x48, 0xb9, 0,    0,    0,    0, 0, 0, 0, 0, // mov rcx,slot
    0x48, 0xb8, 0,    0,    0,    0, 0, 0, 0, 0, // mov rax,gateway
    0xff, 0xd0,                                  // call rax
    0x48, 0x83, 0xc4, 0x28,                      // add rsp,40
    0xc3,                                        // ret
};

// UNWIND_INFO version 1 shared by every gate: 4-byte prologue, one UWOP_ALLOC_SMALL at
// offset 4 with OpInfo=4 (4*8+8 = 40 bytes). The single unwind code is padded to two slots.
constexpr std::array<unsigned char, kUnwindInfoSize> kGateUnwindInfo = {1, 4, 1, 0, 4, 0x42, 0, 0};

void emit_gate(unsigned char* code, NativeHookSlot* slot, void* gateway) {
    std::memcpy(code, kGateTemplate.data(), kGateTemplate.size());
    std::memcpy(code + kGateSlotImmediate, &slot, sizeof slot);
    std::memcpy(code + kGateGatewayImmediate, &gateway, sizeof gateway);
}

std::string last_error_message(const char* operation) {
    const auto code = GetLastError();
    return std::string("Native gate ") + operation + " failed: " + std::to_string(code);
}

struct Chunk {
    std::array<NativeHookSlot, kSlotsPerChunk> slots;
    std::array<RUNTIME_FUNCTION, kGatesPerChunk> functions{};
    // kGateCodeSize bytes of gates followed by the shared UNWIND_INFO.
    unsigned char* code = nullptr;
    size_t used = 0;
    bool registered = false;

    Chunk() = default;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;

    ~Chunk() {
        // Published chunks live in the intentionally immortal arena. Destruction happens only
        // when unpublished construction fails.
        if (registered) {
            RtlDeleteFunctionTable(functions.data());
        }
        if (code) {
            VirtualFree(code, 0, MEM_RELEASE);
        }
    }
};

void emit_chunk_gates(Chunk& chunk) {
    std::memset(chunk.code, 0xcc, kGateCodeSize);
    std::memcpy(chunk.code + kGateCodeSize, kGateUnwindInfo.data(), kGateUnwindInfo.size());
    for (size_t i = 0; i != kSlotsPerChunk; ++i) {
        auto& slot = chunk.slots[i];
        slot.detour_gate = chunk.code + i * kGateStride * 2;
        slot.original_gate = chunk.code + (i * 2 + 1) * kGateStride;
        emit_gate(static_cast<unsigned char*>(slot.detour_gate), &slot,
                  reinterpret_cast<void*>(detour_gateway));
        emit_gate(static_cast<unsigned char*>(slot.original_gate), &slot,
                  reinterpret_cast<void*>(original_gateway));
    }
    for (size_t i = 0; i != kGatesPerChunk; ++i) {
        auto& function = chunk.functions[i];
        function.BeginAddress = static_cast<DWORD>(i * kGateStride);
        function.EndAddress = static_cast<DWORD>(i * kGateStride + kGateSize);
        function.UnwindData = static_cast<DWORD>(kGateCodeSize);
    }
}

std::unique_ptr<Chunk> make_chunk(std::string& error) {
    auto chunk = std::make_unique<Chunk>();
    chunk->code = static_cast<unsigned char*>(VirtualAlloc(
        nullptr, kGateCodeSize + kUnwindInfoSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!chunk->code) {
        error = last_error_message("VirtualAlloc");
        return {};
    }
    emit_chunk_gates(*chunk);
    DWORD previous_protection = 0;
    if (!VirtualProtect(chunk->code, kGateCodeSize + kUnwindInfoSize, PAGE_EXECUTE_READ,
                        &previous_protection)) {
        error = last_error_message("VirtualProtect");
        return {};
    }
    if (!FlushInstructionCache(GetCurrentProcess(), chunk->code, kGateCodeSize)) {
        error = last_error_message("FlushInstructionCache");
        return {};
    }
    if (!RtlAddFunctionTable(chunk->functions.data(), static_cast<DWORD>(kGatesPerChunk),
                             reinterpret_cast<DWORD64>(chunk->code))) {
        error = "Native gate RtlAddFunctionTable failed";
        return {};
    }
    chunk->registered = true;
    return chunk;
}

class SlotArena {
public:
    NativeHookSlot* allocate(void* target, std::string& error) {
        std::lock_guard lock(mutex_);
        if (!next_id_) {
            error = "Native hook ID space exhausted";
            return nullptr;
        }
        if (chunks_.empty() || chunks_.back()->used == kSlotsPerChunk) {
            auto chunk = make_chunk(error);
            if (!chunk) {
                return nullptr;
            }
            chunks_.push_back(std::move(chunk));
        }
        auto& chunk = *chunks_.back();
        auto* slot = &chunk.slots[chunk.used++];
        slot->id = next_id_++;
        slot->target = target;
        return slot;
    }

private:
    std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::vector<std::unique_ptr<Chunk>> chunks_;
};

SlotArena& arena() {
    // Deliberately leaked: neither static destruction nor NativeHooks destruction may
    // invalidate a late native entrant. The pinned core owns this memory until process exit.
    static auto* instance = new SlotArena;
    return *instance;
}

} // namespace

NativeHookSlot* allocate_hook_slot(void* target, std::string& error) {
    return arena().allocate(target, error);
}

} // namespace ark::core
