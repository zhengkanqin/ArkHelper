#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "battle_client.h"

#include <cstring>

namespace arkhelp::ipc {
namespace {

constexpr std::uint64_t kReopenIntervalMs = 1000;

}  // namespace

MappingView::~MappingView() {
    close();
}

bool MappingView::open(const wchar_t* name, std::size_t bytes) {
    close();
    if (name == nullptr || bytes == 0) {
        return false;
    }
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (mapping == nullptr) {
        return false;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes);
    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }
    handle_ = mapping;
    view_ = view;
    bytes_ = bytes;
    return true;
}

void MappingView::close() {
    if (view_ != nullptr) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    bytes_ = 0;
}

bool BattleClient::poll(BattleSnapshot& out) {
    const std::uint64_t now = GetTickCount64();
    if (!mapping_.mapped()) {
        if (now < next_open_ms_) {
            return false;
        }
        next_open_ms_ = now + kReopenIntervalMs;
        mapping_.open(kBattleMappingName, sizeof(BattleSnapshot));
        if (!mapping_.mapped()) {
            return false;
        }
    }
    if (mapping_.bytes() < sizeof(BattleSnapshot)) {
        mapping_.close();
        return false;
    }
    const auto* state = static_cast<const BattleSnapshot*>(mapping_.data());
    if (state->magic != kBattleMagic || state->version != kVersion) {
        // A stale or foreign mapping: drop it and retry on a later frame.
        mapping_.close();
        next_open_ms_ = now + kReopenIntervalMs;
        return false;
    }
    if (state->size < sizeof(BattleSnapshot)) {
        return false;
    }
    if (!read_snapshot(*state, out)) {
        return false;
    }
    if (out.magic != kBattleMagic || out.version != kVersion) {
        return false;
    }
    return true;
}

void BattleClient::close() {
    mapping_.close();
    next_open_ms_ = 0;
}

bool CommandClient::poll(CommandState& out) {
    const std::uint64_t now = GetTickCount64();
    if (!mapping_.mapped()) {
        if (now < next_open_ms_) {
            return false;
        }
        next_open_ms_ = now + kReopenIntervalMs;
        mapping_.open(kCommandMappingName, sizeof(CommandState));
        if (!mapping_.mapped()) {
            return false;
        }
    }
    const auto* state = static_cast<const CommandState*>(mapping_.data());
    if (state->magic != kCommandMagic || state->version != kVersion) {
        mapping_.close();
        next_open_ms_ = now + kReopenIntervalMs;
        return false;
    }
    if (state->size < sizeof(CommandState)) {
        return false;
    }
    if (!read_commands(*state, out)) {
        return false;
    }
    return out.magic == kCommandMagic && out.version == kVersion;
}

void CommandClient::close() {
    mapping_.close();
    next_open_ms_ = 0;
}

}  // namespace arkhelp::ipc
