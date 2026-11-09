#include "../src/game/startup_setup.h"
#include "../src/log/log.h"
#include <windows.h>
#include <shobjidl.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    using namespace arkhelp;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc == 3 && std::wcscmp(argv[1], L"--registered") == 0) {
        log_open(L"build/startup-diagnostics");
        const bool installed = startup::registered_for(argv[2]);
        std::printf("registered_for=%s\n", installed ? "true" : "false");
        log_close();
        CoUninitialize();
        return installed ? 0 : 1;
    }
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    const auto parent = std::filesystem::path(executable).parent_path();
    const auto fixture = parent / (L"启动 配置-" + std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(fixture / L"应用 程序");
        std::filesystem::create_directories(fixture / L"桌面");
        const auto root = fixture / L"应用 程序";
        std::ofstream(root / L"ArkHelper.exe") << "shortcut fixture";
        SetEnvironmentVariableW(L"ARKHELP_NO_GAME_LAUNCH", L"1");
        if (!startup::prepare(root, true, true).empty() || std::filesystem::exists(root / L"runtime"))
            throw std::runtime_error("test mode attempted real environment setup");
        SetEnvironmentVariableW(L"ARKHELP_NO_GAME_LAUNCH", nullptr);
        if (startup::prepare(root).empty()) throw std::runtime_error("incomplete package requested administrator setup");
        const auto error = startup::create_shortcut(root, fixture / L"桌面");
        if (!error.empty()) throw std::runtime_error("native shortcut creation failed");
        IShellLinkW* link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
            throw std::runtime_error("cannot read the generated shortcut");
        IPersistFile* file = nullptr;
        HRESULT result = link->QueryInterface(IID_PPV_ARGS(&file));
        if (SUCCEEDED(result)) result = file->Load((fixture / L"桌面/ArkHelper.lnk").c_str(), STGM_READ);
        if (file) file->Release();
        wchar_t path[32768]{}, icon[32768]{}, work[32768]{}, arguments[128]{};
        int index = -1;
        link->GetPath(path, 32768, nullptr, SLGP_RAWPATH);
        link->GetIconLocation(icon, 32768, &index);
        link->GetWorkingDirectory(work, 32768);
        link->GetArguments(arguments, 128);
        link->Release();
        if (FAILED(result) || path != root / L"ArkHelper.exe" || icon != root / L"ArkHelper.exe" ||
            work != root || index != 0 || arguments[0]) throw std::runtime_error("shortcut target, icon or working directory is incorrect");
        std::puts("PASS native startup: offline gate, incomplete package, Unicode shortcut target/icon/working directory");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
    if (fixture.parent_path() == parent && fixture.filename().wstring().starts_with(L"启动 配置-"))
        std::filesystem::remove_all(fixture);
    CoUninitialize();
    return 0;
}
