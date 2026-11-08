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
    // Returns true only when a consistent, version matching snapshot was copied into `out`.
    bool poll(BattleSnapshot& out);
    void close();
    bool connected() const { return mapping_.mapped(); }

private:
    MappingView mapping_;
    std::uint64_t next_open_ms_ = 0;
};

// Polls the shell-owned command payload (feature switches, card index, UI scale).
class CommandClient {
public:
    bool poll(CommandState& out);
    void close();
    bool connected() const { return mapping_.mapped(); }

private:
    MappingView mapping_;
    std::uint64_t next_open_ms_ = 0;
};

}  // namespace arkhelp::ipc
