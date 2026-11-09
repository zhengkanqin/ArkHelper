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
        have_last_ = false;
        mapping_.open(name_, sizeof(BattleSnapshot));
        if (!mapping_.mapped()) {
            return false;
        }
    }
    if (mapping_.bytes() < sizeof(BattleSnapshot)) {
        have_last_ = false;
        mapping_.close();
        return false;
    }
    const auto* state = static_cast<const BattleSnapshot*>(mapping_.data());
    if (state->magic != kBattleMagic || state->version != kVersion) {
        have_last_ = false;
        // A stale or foreign mapping: drop it and retry on a later frame.
        mapping_.close();
        next_open_ms_ = now + kReopenIntervalMs;
        return false;
    }
    if (state->size < sizeof(BattleSnapshot)) {
        have_last_ = false;
        return false;
    }
    if (!read_snapshot(*state, out)) {
        // Keep a complete sample across a brief writer collision. Its original
        // publication timestamp still controls freshness in every drawing feature.
        if (have_last_ && now >= last_read_ms_ && now-last_read_ms_ <= 200 &&
            state->battle_serial == last_.battle_serial && state->in_battle == last_.in_battle &&
            state->battle_state == last_.battle_state) { out = last_; return true; }
        have_last_ = false;
        return false;
    }
    if (out.magic != kBattleMagic || out.version != kVersion || out.size < sizeof(BattleSnapshot)) {
        have_last_ = false;
        return false;
    }
    last_ = out; last_read_ms_ = now; have_last_ = true;
    return true;
}

void BattleClient::close() {
    have_last_ = false;
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
        have_last_ = false;
        mapping_.open(name_, sizeof(CommandState));
        if (!mapping_.mapped()) {
            return false;
        }
    }
    const auto* state = static_cast<const CommandState*>(mapping_.data());
    if (state->magic != kCommandMagic || state->version != kVersion) {
        have_last_ = false;
        mapping_.close();
        next_open_ms_ = now + kReopenIntervalMs;
        return false;
    }
    if (state->size < sizeof(CommandState)) {
        have_last_ = false;
        return false;
    }
    if (!read_commands(*state, out)) {
        if (have_last_ && now >= last_read_ms_ && now-last_read_ms_ <= 200 &&
            state->revision == last_.revision) { out = last_; return true; }
        have_last_ = false;
        return false;
    }
    if (out.magic != kCommandMagic || out.version != kVersion || out.size < sizeof(CommandState)) {
        have_last_ = false; return false;
    }
    last_ = out; last_read_ms_ = now; have_last_ = true;
    return true;
}

void CommandClient::close() {
    have_last_ = false;
    mapping_.close();
    next_open_ms_ = 0;
}

}  // namespace arkhelp::ipc
