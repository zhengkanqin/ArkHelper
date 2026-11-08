// Exercise the production desktop writer and the provider's shared gate contract.
// Win32 mapping APIs below use one private POD buffer: this executable never
// creates, opens or changes an OS mapping or a running game's command state.
#include "../src/shell/ipc_client.h"
#include "../src/log/log.h"
#include "../src/shell/win_util.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <stdexcept>

namespace fixture {
arkhelp::ipc::CommandState state{};
bool exists = false;
DWORD last_error = ERROR_SUCCESS;
int handles = 0, views = 0, checks = 0;

void check(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
HANDLE WINAPI create_mapping(HANDLE file, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD bytes, LPCWSTR name) {
    check(file == INVALID_HANDLE_VALUE && bytes == arkhelp::ipc::kCommandMappingBytes,
          "desktop must create its expected command mapping layout");
    check(name && std::wcscmp(name, arkhelp::ipc::kCommandMappingName) == 0,
          "unexpected mapping request in isolated fixture");
    last_error = exists ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS;
    exists = true;
    ++handles;
    return static_cast<HANDLE>(&state);
}
HANDLE WINAPI open_mapping(DWORD, BOOL, LPCWSTR) {
    throw std::runtime_error("test attempted to open a real/battle mapping");
}
LPVOID WINAPI map_view(HANDLE mapping, DWORD, DWORD, DWORD, SIZE_T bytes) {
    check(mapping == &state && bytes == sizeof(state), "unexpected command mapping view");
    ++views;
    return &state;
}
BOOL WINAPI unmap_view(LPCVOID address) {
    check(address == &state && views > 0, "unbalanced command unmap");
    --views;
    return TRUE;
}
BOOL WINAPI close_handle(HANDLE handle) {
    check(handle == &state && handles > 0, "unbalanced command handle close");
    --handles;
    return TRUE;
}
DWORD WINAPI get_last_error() { return last_error; }
} // namespace fixture

// Include the implementation, not a reimplementation of its feature writes.
// Windows headers are already parsed, so only these source-level API calls change.
#define CreateFileMappingW fixture::create_mapping
#define OpenFileMappingW fixture::open_mapping
#define MapViewOfFile fixture::map_view
#define UnmapViewOfFile fixture::unmap_view
#define CloseHandle fixture::close_handle
#define GetLastError fixture::get_last_error
#include "../src/shell/ipc_client.cpp"
#undef CreateFileMappingW
#undef OpenFileMappingW
#undef MapViewOfFile
#undef UnmapViewOfFile
#undef CloseHandle
#undef GetLastError

namespace arkhelp {
// Logging is irrelevant to the command contract and must not write files here.
void log_info(const char*, ...) {}
void log_warn(const char*, ...) {}
void log_error(const char*, ...) {}
std::string wide_to_utf8(const std::wstring&) { return {}; }
} // namespace arkhelp

namespace {
using namespace arkhelp;
using namespace arkhelp::ipc;
using fixture::check;

CommandState read_command() {
    CommandState command;
    check(read_commands(fixture::state, command), "provider must read a coherent command");
    check(command.magic == kCommandMagic && command.version == kVersion && command.size == sizeof(command),
          "provider must accept command magic/version/layout");
    return command;
}
void gates(const CommandState& command, std::uint32_t expected) {
    for (auto feature : {kFeatureEnemyRange, kFeatureOperatorStats, kFeatureEnemyPaths})
        check(command_feature_enabled(command, feature) == ((expected & feature) != 0),
              "desktop feature selection did not reach the provider capture gate");
}
void empty_requests(const CommandState& command) {
    check(command.path_request_battle_serial == 0 && command.path_request_count == 0 &&
              std::all_of(std::begin(command.path_request_ids), std::end(command.path_request_ids),
                          [](auto id) { return id == 0; }),
          "disabled path capture retained pending instance requests");
}
template <class Mutation>
void seed_legacy(Mutation mutate) {
    begin_write(fixture::state.sequence);
    mutate(fixture::state);
    end_write(fixture::state.sequence);
}
} // namespace

int main() {
    try {
        CommandClient desktop;
        check(desktop.open() && desktop.ready(), "fresh desktop writer did not open");
        auto command = read_command();
        gates(command, 0);
        check(!(command.flags & kCommandOverlayEnabled), "fresh zero mask unexpectedly enabled capture");
        empty_requests(command);

        // Regression: each of these gates used to stay false because flags was
        // permanently initialized to zero despite successful feature-mask writes.
        for (auto feature : {kFeatureEnemyRange, kFeatureOperatorStats, kFeatureEnemyPaths}) {
            check(desktop.set_feature_mask(feature), "single-feature mask was not written");
            command = read_command();
            check(command.flags & kCommandOverlayEnabled, "nonzero feature mask left capture disabled");
            gates(command, feature);
        }
        check(desktop.set_feature_bit(kFeatureOperatorStats, true), "operator toggle did not write");
        gates(read_command(), kFeatureOperatorStats | kFeatureEnemyPaths);
        const auto enabled = read_command();
        check(!desktop.set_feature_mask(enabled.feature_mask), "unchanged valid mask should be a no-op");
        check(desktop.revision() == enabled.revision && desktop.sequence() == enabled.sequence,
              "unchanged valid mask needlessly changed revision/seqlock");

        // Attach to legacy state with the same desired mask: the early no-op
        // check must also examine flags and repair the missing global enable bit.
        seed_legacy([](auto& legacy) {
            legacy.flags = kCommandReloadDataPack | (1u << 12);
        });
        const auto legacy_revision = desktop.revision();
        check(desktop.set_feature_mask(enabled.feature_mask), "equal mask failed to repair legacy flags");
        command = read_command();
        gates(command, enabled.feature_mask);
        check(command.flags == (kCommandOverlayEnabled | kCommandReloadDataPack | (1u << 12)) &&
                  command.revision == legacy_revision + 1,
              "enable repair lost other flags or failed to revise command");

        std::array<std::uint64_t, 4> ids{41, 0, 41, 42};
        check(desktop.set_path_requests(7, ids), "enabled path requests were rejected");
        command = read_command();
        check(command.path_request_battle_serial == 7 && command.path_request_count == 2 &&
                  command.path_request_ids[0] == 41 && command.path_request_ids[1] == 42,
              "path request normalization lost instance identities");
        check(desktop.set_feature_bit(kFeatureEnemyPaths, false), "path toggle did not turn off");
        command = read_command();
        gates(command, kFeatureOperatorStats);
        empty_requests(command);
        check(!desktop.set_path_requests(7, ids), "disabled path feature accepted new requests");
        empty_requests(read_command());

        check(desktop.set_feature_bit(kFeatureOperatorStats, false), "last feature did not turn off");
        command = read_command();
        gates(command, 0);
        check(!(command.flags & kCommandOverlayEnabled) &&
                  command.flags == (kCommandReloadDataPack | (1u << 12)),
              "zero feature mask failed to disable capture while preserving other flags");

        seed_legacy([](auto& legacy) {
            legacy.flags |= kCommandOverlayEnabled;
            legacy.path_request_battle_serial = 9;
            legacy.path_request_count = 1;
            legacy.path_request_ids[0] = 99;
        });
        check(desktop.set_feature_mask(0), "equal zero mask failed to repair stale enable/request state");
        command = read_command();
        check(!(command.flags & kCommandOverlayEnabled), "zero-mask repair retained capture enable");
        empty_requests(command);

        // Provider gate is a conjunction: either half alone cannot start work.
        CommandState gate_fixture{};
        gate_fixture.feature_mask = kFeatureEnemyRange | kFeatureOperatorStats | kFeatureEnemyPaths;
        gates(gate_fixture, 0);
        gate_fixture.flags = kCommandOverlayEnabled;
        gates(gate_fixture, gate_fixture.feature_mask);
        gate_fixture.feature_mask = 0;
        gates(gate_fixture, 0);

        check(desktop.set_feature_mask(kFeatureEnemyPaths), "path re-enable failed");
        std::array<std::uint64_t,kMaxUnits> group{};
        for(std::size_t i=0;i<group.size();++i) group[i]=100+i;
        check(desktop.set_path_requests(11,group), "overlapping group request was rejected");
        command=read_command();
        check(command.path_request_count==kMaxUnits && command.path_request_ids[kMaxUnits-1]==163,
              "command IPC truncated overlapping enemies to the former sixteen-path capacity");
        check(desktop.set_path_requests(11, ids), "path re-enable did not accept requests");
        const auto before_close = read_command();
        CommandClient temporary;
        check(temporary.open(), "second writer could not attach to existing mapping");
        temporary.close();
        command = read_command();
        check(command.flags == before_close.flags && command.feature_mask == before_close.feature_mask &&
                  command.path_request_count == before_close.path_request_count &&
                  command.revision == before_close.revision,
              "short-lived client close damaged the active desktop session");
        desktop.close();
        check(!desktop.ready() && fixture::handles == 0 && fixture::views == 0,
              "isolated writer leaked mapping handles/views");
        check(fixture::state.flags == before_close.flags, "close unexpectedly cleared the active enable flag");
        std::printf("PASS production CommandClient/provider feature gate: %d checks, private memory only\n", fixture::checks);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL command gate: %s\n", error.what());
        return 1;
    }
}
