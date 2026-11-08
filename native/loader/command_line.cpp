#include "command_line.h"

#include <wchar.h>

#include <optional>

namespace ark::loader {

namespace {

constexpr unsigned long long kMaxTimeoutSeconds = 600;

// Accepts a decimal value in [1, max]; trailing characters are rejected.
std::optional<DWORD> parse_positive(const wchar_t* text, unsigned long long max) {
    wchar_t* end = nullptr;
    const unsigned long long value = wcstoull(text, &end, 10);
    if (!value || value > max || !end || *end) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

bool is_option(const wchar_t* argument, const wchar_t* name) {
    return wcscmp(argument, name) == 0;
}

} // namespace

bool parse_command_line(int argc, wchar_t** argv, CommandLineOptions& options) {
    bool valid = true;
    bool method_set = false;
    bool timeout_set = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* argument = argv[i];
        const bool has_value = i + 1 < argc;
        if (is_option(argument, L"--pid") && has_value) {
            const auto pid = parse_positive(argv[++i], MAXDWORD);
            if (options.pid || !pid) {
                valid = false;
            } else {
                options.pid = *pid;
            }
        } else if (is_option(argument, L"--timeout-seconds") && has_value) {
            const auto seconds = parse_positive(argv[++i], kMaxTimeoutSeconds);
            if (timeout_set || !seconds) {
                valid = false;
            } else {
                options.timeout_seconds = *seconds;
            }
            timeout_set = true;
        } else if (is_option(argument, L"--report") && has_value) {
            if (!options.report_path.empty()) {
                valid = false;
            }
            options.report_path = argv[++i];
        } else if (is_option(argument, L"--self-test")) {
            if (options.self_test) {
                valid = false;
            }
            options.self_test = true;
        } else if (is_option(argument, L"--watch")) {
            if (options.watch) {
                valid = false;
            }
            options.watch = true;
        } else if (is_option(argument, L"--diagnose")) {
            if (options.diagnose) {
                valid = false;
            }
            options.diagnose = true;
        } else if (is_option(argument, L"--method") && has_value) {
            if (method_set) {
                valid = false;
            }
            method_set = true;
            const wchar_t* method = argv[++i];
            if (is_option(method, L"remote-thread")) {
                options.method = LoadMethod::RemoteThread;
            } else if (is_option(method, L"window-hook")) {
                options.method = LoadMethod::WindowHook;
            } else {
                valid = false;
            }
        } else {
            valid = false;
        }
    }

    // Exactly one target selection: --pid, or --watch and/or --self-test.
    const bool selects_by_pid = options.pid != 0;
    const bool selects_by_launch = options.watch || options.self_test;
    if (selects_by_pid == selects_by_launch) {
        valid = false;
    }
    if (options.diagnose && (!selects_by_pid || selects_by_launch || method_set)) {
        valid = false;
    }
    if (!valid) {
        return false;
    }
    if (options.watch && !method_set) {
        options.method = LoadMethod::RemoteThread;
    }
    return true;
}

} // namespace ark::loader
