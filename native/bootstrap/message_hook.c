// ArkHelperBootstrap.dll: an explicitly named bootstrap for the documented WH_GETMESSAGE
// and WH_CALLWNDPROC hook APIs (the latter also wakes minimized, sent-message-only loops).
// USER32 maps it into the hooked thread's process; it does not replace a system DLL or
// conceal a module. On the first hooked message it loads and starts ArkHelperRuntime.dll from
// its own directory, writes `logs\ArkHelperBootstrap-<pid>.txt` ("<pid> <module> <error>") and
// signals `Local\ArkHelp.Runtime.HookReady.<pid>` for the loader.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>

#define ARK_MAX_PATH_CHARS 32768
// Directory paths must leave room for the file names appended to them.
#define ARK_MAX_DIRECTORY_CHARS 32500

typedef bool (*ArkHelperRuntimeStartFn)(const void*);

static HMODULE self_module;
static volatile LONG load_requested;

// Writes this DLL's directory, including the trailing separator, into `path`.
static BOOL get_module_directory(WCHAR* path) {
    DWORD length = GetModuleFileNameW(self_module, path, ARK_MAX_PATH_CHARS);
    if (length == 0 || length >= ARK_MAX_DIRECTORY_CHARS) {
        return FALSE;
    }
    while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/') {
        --length;
    }
    path[length] = 0;
    return TRUE;
}

static void write_result_file(HMODULE core, DWORD error) {
    WCHAR path[ARK_MAX_PATH_CHARS];
    WCHAR file_name[96];
    if (!get_module_directory(path)) {
        return;
    }
    wsprintfW(file_name, L"logs\\ArkHelperBootstrap-%lu.txt", GetCurrentProcessId());
    lstrcatW(path, file_name);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char text[128];
    int length = snprintf(text, sizeof(text), "%lu %llu %lu\n", GetCurrentProcessId(),
                          (unsigned long long)(ULONG_PTR)core, error);
    DWORD written = 0;
    WriteFile(file, text, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

static void signal_hook_ready(void) {
    WCHAR event_name[96];
    wsprintfW(event_name, L"Local\\ArkHelp.Runtime.HookReady.%lu", GetCurrentProcessId());
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
}

// Returns the started core module, or NULL with `*error` describing the failure.
static HMODULE load_and_start_core(DWORD* error) {
    WCHAR path[ARK_MAX_PATH_CHARS];
    if (!get_module_directory(path)) {
        *error = ERROR_BAD_PATHNAME;
        return NULL;
    }
    lstrcatW(path, L"ArkHelperRuntime.dll");
    HMODULE core = LoadLibraryExW(
        path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!core) {
        *error = GetLastError();
        return NULL;
    }
    // Casting through void (*)(void) is the conventional FARPROC conversion.
    ArkHelperRuntimeStartFn start =
        (ArkHelperRuntimeStartFn)(void (*)(void))GetProcAddress(core, "arkhelp_runtime_start");
    if (!start) {
        *error = ERROR_PROC_NOT_FOUND;
        return NULL;
    }
    if (!start(NULL)) {
        *error = GetLastError();
        if (*error == 0) {
            *error = ERROR_DLL_INIT_FAILED;
        }
        return NULL;
    }
    *error = 0;
    return core;
}

__declspec(dllexport) LRESULT CALLBACK ArkHelperBootstrapHook(int code, WPARAM wparam, LPARAM lparam) {
    WCHAR event_name[96];
    wsprintfW(event_name, L"Local\\ArkHelp.Runtime.HookReady.%lu", GetCurrentProcessId());
    HANDLE request = code >= 0 ? OpenEventW(SYNCHRONIZE, FALSE, event_name) : NULL;
    if (request && WaitForSingleObject(request, 0) == WAIT_TIMEOUT &&
        InterlockedCompareExchange(&load_requested, 1, 0) == 0) {
        // USER32 may drop its hook reference as soon as the loader unhooks. Keep this
        // bootstrap mapped for the lifetime of the target process.
        HMODULE pinned = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)ArkHelperBootstrapHook, &pinned);
        DWORD error = 0;
        HMODULE core = load_and_start_core(&error);
        write_result_file(core, error);
        signal_hook_ready();
        InterlockedExchange(&load_requested, 0);
    }
    if (request) { CloseHandle(request); }
    return CallNextHookEx(NULL, code, wparam, lparam);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = module;
    }
    return TRUE;
}
