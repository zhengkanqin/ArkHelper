// Read-only views over the ArkHelper IPC shared memory sections.
//
// ArkHelper.exe never writes the battle snapshot: the in-game provider owns that mapping. The command
// section is owned by the shell (harness/menu) code, so this file only reads it here. All calls are
// lock free and cheap enough to run once per rendered frame on the UI thread.
#pragma once

#include <cstddef>
#include <cstdint>

#include "arkhelp/ipc.h"

namespace arkhelp::ipc {

// Opens a named file mapping read-only and exposes the mapped bytes.
class MappingView {
public:
    MappingView() = default;
    ~MappingView();

    MappingView(const MappingView&) = delete;
    MappingView& operator=(const MappingView&) = delete;

    bool open(const wchar_t* name, std::size_t bytes);
    void close();

    bool mapped() const { return view_ != nullptr; }
    const void* data() const { return view_; }
    std::size_t bytes() const { return bytes_; }

private:
    void* handle_ = nullptr;
    void* view_ = nullptr;
    std::size_t bytes_ = 0;
};

// Polls the provider-owned battle snapshot. A missing provider (game not running or plugin not
// loaded) is not an error: poll() returns false and the overlay simply renders nothing.
class BattleClient {
public:
    explicit BattleClient(const wchar_t* name = kBattleMappingName) : name_(name) {}
    // Brief writer collisions reuse the last coherent snapshot for at most 200 ms,
    // preserving its publication time and refusing changed battle identities/states.
    bool poll(BattleSnapshot& out);
    void close();
    bool connected() const { return mapping_.mapped(); }

private:
    MappingView mapping_;
    std::uint64_t next_open_ms_ = 0;
    const wchar_t* name_;
    BattleSnapshot last_{};
    std::uint64_t last_read_ms_ = 0;
    bool have_last_ = false;
};

// Polls the shell-owned command payload (feature switches, card index, UI scale).
class CommandClient {
public:
    explicit CommandClient(const wchar_t* name = kCommandMappingName) : name_(name) {}
    bool poll(CommandState& out);
    void close();
    bool connected() const { return mapping_.mapped(); }

private:
    MappingView mapping_;
    std::uint64_t next_open_ms_ = 0;
    const wchar_t* name_;
    CommandState last_{};
    std::uint64_t last_read_ms_ = 0;
    bool have_last_ = false;
};

}  // namespace arkhelp::ipc
