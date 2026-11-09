#include "startup_setup.h"
#include "../launcher/agent_protocol.h"
#include "../log/log.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <initguid.h>
#include <taskschd.h>
#include <fstream>

namespace arkhelp::startup {
namespace {
template<class T> struct Com {
    T* value = nullptr;
    ~Com() { if (value) value->Release(); }
    T** out() { return &value; }
    T* operator->() const { return value; }
};
struct BString {
    BSTR value;
    explicit BString(const wchar_t* text) : value(SysAllocString(text)) {}
    ~BString() { SysFreeString(value); }
};

bool task_call(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return true;
    log_warn("startup task %s failed: 0x%08lX", operation, static_cast<unsigned long>(result));
    return false;
}

DWORD install_agent(const std::filesystem::path& root) {
    const auto executable = (root / L"ArkHelperLauncher.exe").wstring();
    const auto directory = root.wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = L"--install-agent";
    info.lpDirectory = directory.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return GetLastError();
    const ULONGLONG deadline = GetTickCount64() + 120000;
    DWORD result = WAIT_TIMEOUT;
    while (GetTickCount64() < deadline) {
        const DWORD waited = MsgWaitForMultipleObjects(1, &info.hProcess, FALSE, 100, QS_ALLINPUT);
        if (waited == WAIT_OBJECT_0) {
            if (!GetExitCodeProcess(info.hProcess, &result)) result = GetLastError();
            break;
        }
        if (waited == WAIT_FAILED) { result = GetLastError(); break; }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                CloseHandle(info.hProcess);
                return ERROR_CANCELLED;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CloseHandle(info.hProcess);
    return result;
}
}

bool registered_for(const std::filesystem::path& root) {
    Com<ITaskService> service;
    if (!task_call(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
            IID_ITaskService, reinterpret_cast<void**>(service.out())), "create")) return false;
    VARIANT empty{};
    if (!task_call(service->Connect(empty, empty, empty, empty), "connect")) return false;
    Com<ITaskFolder> folder;
    BString folder_name(L"\\"), task_name(L"ArkHelper Watch Agent");
    if (!task_call(service->GetFolder(folder_name.value, folder.out()), "folder")) return false;
    Com<IRegisteredTask> task;
    if (!task_call(folder->GetTask(task_name.value, task.out()), "get")) return false;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (!task_call(task->get_Enabled(&enabled), "enabled") || !enabled) return false;
    Com<ITaskDefinition> definition;
    Com<IActionCollection> actions;
    if (!task_call(task->get_Definition(definition.out()), "definition") ||
        !task_call(definition->get_Actions(actions.out()), "actions")) return false;
    LONG count = 0;
    if (!task_call(actions->get_Count(&count), "count") || count != 1) return false;
    Com<IAction> action;
    Com<IExecAction> execution;
    if (!task_call(actions->get_Item(1, action.out()), "action") ||
        !task_call(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(execution.out())), "exec")) return false;
    BSTR path = nullptr, arguments = nullptr;
    const bool read = SUCCEEDED(execution->get_Path(&path)) && SUCCEEDED(execution->get_Arguments(&arguments));
    std::wstring actual = path ? path : L"";
    if (actual.size() > 1 && actual.front() == L'"' && actual.back() == L'"')
        actual = actual.substr(1, actual.size() - 2);
    const bool matches = read && _wcsicmp(actual.c_str(), (root / L"ArkHelperLauncher.exe").c_str()) == 0 &&
        arguments && std::wcscmp(arguments, L"--agent") == 0;
    if (!matches) log_warn("startup task action differs from this installation");
    SysFreeString(path); SysFreeString(arguments);
    return matches;
}

std::wstring create_shortcut(const std::filesystem::path& root, const std::filesystem::path& destination) {
    std::filesystem::path directory = destination;
    if (directory.empty()) {
        PWSTR desktop = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
            return L"无法定位桌面目录";
        directory = desktop;
        CoTaskMemFree(desktop);
    }
    Com<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(link.out())))) return L"无法创建桌面快捷方式";
    const auto executable = (root / L"ArkHelper.exe").wstring();
    if (FAILED(link->SetPath(executable.c_str())) || FAILED(link->SetArguments(L"")) ||
        FAILED(link->SetWorkingDirectory(root.c_str())) || FAILED(link->SetIconLocation(executable.c_str(), 0)) ||
        FAILED(link->SetShowCmd(SW_SHOWNORMAL)) || FAILED(link->SetDescription(L"ArkHelper")))
        return L"快捷方式设置失败";
    Com<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.out()))) ||
        FAILED(file->Save((directory / L"ArkHelper.lnk").c_str(), TRUE)))
        return L"无法保存桌面快捷方式，可在设置中重试";
    return {};
}

std::wstring prepare(const std::filesystem::path& root, bool repair, bool first_use) {
    if (launcher::game_launch_disabled()) return {};
    for (const auto* name : {L"ArkHelperLauncher.exe", L"runtime/ArkHelperRuntime.dll", L"runtime/ArkHelperBootstrap.dll"}) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(root / name, error))
            return L"程序文件不完整，请完整解压程序包后启动 ArkHelper.exe";
    }
    const bool installed = registered_for(root);
    if (repair || !installed) {
        const DWORD code = install_agent(root);
        if (code == ERROR_CANCELLED) return L"已取消管理员授权，可点击“修复启动环境”重试";
        if (code != 0 || !registered_for(root))
            return L"启动环境配置失败（" + std::to_wstring(code) + L"），请在设置中重试修复";
        log_info("startup environment installed for this application directory");
    }
    std::ofstream(root / L"runtime/watch-agent.installed") << "installed\n";
    if (first_use || !installed) {
        const auto error = create_shortcut(root);
        if (!error.empty()) log_warn("desktop shortcut creation failed");
    }
    return {};
}
}
