<#
.SYNOPSIS
Builds the complete ArkHelper product from this directory alone.
.DESCRIPTION
All builds both GUIs, the private runtime and bootstrap. Tests runs the console
regression suite. Runtime rebuilds the private runtime; App rebuilds both GUIs.
#>
[CmdletBinding()]
param(
    [ValidateSet('All', 'App', 'Runtime', 'Tests')][string]$Target = 'All',
    [string]$DistDirectory = 'dist',
    [switch]$Release,
    [switch]$SkipTests,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = $PSScriptRoot
$Build = Join-Path $Root $(if ($Release) { 'build\release' } else { 'build\debug' })
$Dist = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($DistDirectory)) {
    $DistDirectory
} else { Join-Path $Root $DistDirectory }))
if ($Clean -and (Test-Path -LiteralPath $Build)) {
    $resolvedBuild = [IO.Path]::GetFullPath($Build)
    if (-not $resolvedBuild.StartsWith($Root.TrimEnd('\') + '\build\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing to clean a directory outside ArkHelper/build.'
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}
$Objects = Join-Path $Build 'obj'
$Tests = Join-Path $Build 'tests'
$Runtime = Join-Path $Dist 'runtime'
foreach ($directory in @($Objects, $Tests, $Runtime)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$TempDirectory = Join-Path $Build 'tmp'
New-Item -ItemType Directory -Path $TempDirectory -Force | Out-Null
$env:TEMP = $TempDirectory
$env:TMP = $TempDirectory
$bundledZig = Join-Path $Root '.tools\zig\ziglang\zig.exe'
$zigCommand = Get-Command zig -ErrorAction SilentlyContinue
if (Test-Path -LiteralPath $bundledZig) { $Zig = $bundledZig }
elseif ($zigCommand) { $Zig = $zigCommand.Source }
else { throw 'Put Zig 0.15.1 on PATH or in ArkHelper/.tools/zig/ziglang.' }
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Root 'build\zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $Build 'zig-cache'
$BaseFlags = @('-target', 'x86_64-windows-gnu', '-O2')
if ($Release) { $BaseFlags += '-g0' }
$Defines = @('-DNOMINMAX', '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS')
$Includes = @('include', 'src', 'native\include', 'native\core', 'native\modules\provider',
    'native\modules\common', 'tests\native\fixtures', 'third_party\minhook-1.3.4\include') | ForEach-Object { '-I' + (Join-Path $Root $_) }
$CppFlags = @('-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter',
    '-Wno-missing-field-initializers', '-Wno-cast-function-type-mismatch') + $Defines + $Includes
$NewestHeader = Get-ChildItem -LiteralPath (Join-Path $Root 'src'), (Join-Path $Root 'native'),
    (Join-Path $Root 'include'), (Join-Path $Root 'tests'), (Join-Path $Root 'third_party') -Recurse -File -Filter '*.h' |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty LastWriteTimeUtc
$ScriptTime = (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc
function Invoke-Zig([string[]]$Arguments) {
    & $Zig @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "zig $($Arguments[0]) failed (exit $LASTEXITCODE)." }
}
function Get-Sources([string]$Directory, [switch]$Recurse) {
    Get-ChildItem -LiteralPath (Join-Path $Root $Directory) -File -Filter '*.cpp' -Recurse:$Recurse |
        Sort-Object FullName | ForEach-Object { $_.FullName.Substring($Root.Length + 1) }
}
function Compile([string]$Source, [string]$Tag = '', [string[]]$ExtraFlags = @(), [string[]]$Dependencies = @()) {
    $inputPath = Join-Path $Root $Source
    $object = Join-Path $Objects (($Source -replace '[\\/.]', '_') + $Tag + '.o')
    $flags = $BaseFlags + $Includes + $Defines
    $driver = 'cc'
    if (-not $Source.EndsWith('.c')) { $driver = 'c++'; $flags += $CppFlags }
    $flags += $ExtraFlags
    $signature = $flags -join "`n"
    $commandFile = $object + '.flags'
    $needsBuild = -not (Test-Path -LiteralPath $object)
    if (-not $needsBuild) {
        $objectTime = (Get-Item -LiteralPath $object).LastWriteTimeUtc
        $needsBuild = (Get-Item -LiteralPath $inputPath).LastWriteTimeUtc -gt $objectTime -or
            $NewestHeader -gt $objectTime -or $ScriptTime -gt $objectTime -or
            -not (Test-Path -LiteralPath $commandFile)
        if (-not $needsBuild) {
            foreach ($dependency in $Dependencies) {
                if ((Get-Item -LiteralPath (Join-Path $Root $dependency)).LastWriteTimeUtc -gt $objectTime) {
                    $needsBuild = $true
                    break
                }
            }
        }
        if (-not $needsBuild) { $needsBuild = [IO.File]::ReadAllText($commandFile) -cne $signature }
    }
    if ($needsBuild) {
        Write-Host "Compile $Source $Tag"
        Invoke-Zig (@($driver) + $flags + @('-c', $inputPath, '-o', $object))
        [IO.File]::WriteAllText($commandFile, $signature)
    }
    return $object
}
function Compile-All([string[]]$Sources) { foreach ($source in $Sources) { Compile $source } }
function Link([string]$Output, [string[]]$Inputs, [string[]]$Libraries = @(), [switch]$Dll, [switch]$Gui, [switch]$Wide) {
    Write-Host "Link $Output"
    $flags = $BaseFlags
    if ($Release) { $flags += '-s' }
    if ($Dll) { $flags += '-shared' }
    if ($Gui) { $flags += @('-mwindows', '-Wl,--subsystem,windows') }
    if ($Wide) { $flags += '-municode' }
    Invoke-Zig (@('c++') + $flags + $Inputs + $Libraries + @('-o', $Output))
    if ($Gui) {
        $bytes = [IO.File]::ReadAllBytes($Output)
        $pe = [BitConverter]::ToInt32($bytes, 60)
        if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 2) {
            throw "GUI executable has the wrong PE subsystem: $Output"
        }
    }
}
function Run-Test([string]$Name, [string[]]$Arguments = @()) {
    & (Join-Path $Tests ($Name + '.exe')) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE)." }
}
function Copy-Contents([string]$Source, [string]$Destination) {
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source | Copy-Item -Destination $Destination -Recurse -Force
}
$Core = @(Get-Sources 'native\core')
$NativeHooks = @($Core | Where-Object { (Split-Path $_ -Leaf) -like 'native_hook*.cpp' })
$Lifecycle = @($Core | Where-Object { (Split-Path $_ -Leaf) -ne 'runtime.cpp' })
if ($Target -ne 'App') {
    $MinHookObjects = @(Compile-All @('third_party\minhook-1.3.4\src\buffer.c',
        'third_party\minhook-1.3.4\src\hook.c', 'third_party\minhook-1.3.4\src\trampoline.c',
        'third_party\minhook-1.3.4\src\hde\hde64.c'))
}
if ($Target -eq 'Tests' -or ($Target -eq 'All' -and -not $SkipTests)) {
    Link (Join-Path $Tests 'damage_meter_tests.exe') @(Compile 'tests\native\damage_meter_tests.cpp') -Wide
    $fixture = 'tests\native\fixtures\fixture_plugin.cpp'
    Link (Join-Path $Tests 'fixture.dll') @(Compile $fixture) -Dll
    Link (Join-Path $Tests 'bad_abi.dll') @(Compile $fixture '_bad_abi' @('-DTEST_BAD_ABI')) -Dll
    $names = @('provider', 'consumer', 'intermediate', 'indirect', 'undeclared', 'cycle_a', 'cycle_b',
        'missing_dependency', 'unsafe_dependency', 'failed_init', 'lifetime_provider', 'failed_init_refuse')
    for ($kind = 1; $kind -le $names.Count; ++$kind) {
        Link (Join-Path $Tests ($names[$kind - 1] + '.dll')) @(
            Compile 'tests\native\fixtures\lifecycle_service_fixture.cpp' "_kind$kind" @("-DFIXTURE_KIND=$kind")) -Dll
    }
    Link (Join-Path $Tests 'consumer_updated.dll') @(
        Compile 'tests\native\fixtures\lifecycle_service_fixture.cpp' '_updated' @('-DFIXTURE_KIND=2', '-DFIXTURE_ADDED_DEPENDENCY')) -Dll
    Link (Join-Path $Tests 'native_hook_fixture.dll') @(Compile 'tests\native\fixtures\native_hook_fixture.cpp') -Dll
    # Dynamic modules and legacy input adapters are compiled only into isolated
    # lifecycle tests. Distinct objects prevent these flags leaking into runtime.
    $lifecycleObjects = @(foreach ($source in $Lifecycle) {
        Compile $source '_test_modules' @('-DARKHELP_ENABLE_TEST_MODULES=1')
    }) + $MinHookObjects
    Link (Join-Path $Tests 'lifecycle_tests.exe') (@(Compile 'tests\native\lifecycle_tests.cpp') + $lifecycleObjects) -Wide
    Link (Join-Path $Tests 'native_hook_tests.exe') (@(Compile 'tests\native\native_hook_tests.cpp') + @(Compile-All $NativeHooks) + $MinHookObjects) -Wide
    Link (Join-Path $Tests 'builtin_module_tests.exe') (@(Compile 'tests\builtin_module_tests.cpp') + @(Compile-All $Lifecycle) + $MinHookObjects) -Wide
    Link (Join-Path $Tests 'production_statistics_tests.exe') (@(Compile 'tests\production_statistics_tests.cpp') +
        @(Compile-All $Lifecycle) + $MinHookObjects + @(Compile-All @(Get-Sources 'native\modules\provider' -Recurse)) +
        @(Compile 'native\modules\common\il2cpp_runtime.cpp'; Compile 'src\ipc\ipc.cpp')) @('-luser32') -Wide
    Link (Join-Path $Tests 'ipc_tests.exe') @(Compile 'tests\ipc_tests.cpp'; Compile 'src\ipc\ipc.cpp') -Wide
    Link (Join-Path $Tests 'feature_command_gate_tests.exe') @(
        Compile 'tests\feature_command_gate_tests.cpp' '' @() @('src\shell\ipc_client.cpp')
        Compile 'src\ipc\ipc.cpp')
    Link (Join-Path $Tests 'enemy_publication_tests.exe') @(Compile 'tests\enemy_publication_tests.cpp')
    Link (Join-Path $Tests 'motion_predictor_tests.exe') @(Compile 'tests\motion_predictor_tests.cpp')
    Link (Join-Path $Tests 'overlay_range_tests.exe') @(Compile 'tests\overlay_range_tests.cpp')
    Link (Join-Path $Tests 'overlay_window_geometry_tests.exe') @(
        Compile 'tests\overlay_window_geometry_tests.cpp'; Compile 'src\features\features.cpp';
        Compile 'src\gfx\gfx.cpp'; Compile 'src\ipc\battle_client.cpp'; Compile 'src\ipc\ipc.cpp';
        Compile 'src\log\log.cpp'; Compile 'src\shell\win_util.cpp') @(
        '-luser32', '-lgdi32', '-ld2d1', '-ldwrite', '-lshell32', '-lole32', '-luuid',
        '-lwindowscodecs', '-lmsimg32', '-ldwmapi', '-lshlwapi', '-ladvapi32') -Wide
    Link (Join-Path $Tests 'range_projection_tests.exe') @(Compile 'tests\range_projection_tests.cpp')
    Link (Join-Path $Tests 'panel_session_tests.exe') @(Compile 'tests\panel_session_tests.cpp')
    Link (Join-Path $Tests 'battle_clock_tests.exe') @(Compile 'tests\battle_clock_tests.cpp')
    Link (Join-Path $Tests 'menu_viewport_tests.exe') @(Compile 'tests\menu_viewport_tests.cpp')
    Link (Join-Path $Tests 'prts_client_tests.exe') @(
        Compile 'tests\prts_client_tests.cpp'; Compile 'src\net\prts.cpp'; Compile 'src\net\prts_cache.cpp')
    Link (Join-Path $Tests 'prts_cache_tests.exe') @(
        Compile 'tests\prts_cache_tests.cpp'; Compile 'src\net\prts.cpp'; Compile 'src\net\prts_cache.cpp')
    Link (Join-Path $Tests 'path_animation_tests.exe') @(Compile 'tests\path_animation_tests.cpp')
    Link (Join-Path $Tests 'path_capture_tests.exe') @(Compile 'tests\path_capture_tests.cpp')
    Link (Join-Path $Tests 'operator_capture_tests.exe') @(
        Compile 'tests\operator_capture_tests.cpp'; Compile 'native\modules\common\il2cpp_runtime.cpp')
    Link (Join-Path $Tests 'autochess_statistics_tests.exe') @(
        Compile 'tests\autochess_statistics_tests.cpp' '' @() @('native\modules\damage_meter\native_capture.cpp'))
    Link (Join-Path $Tests 'live_path_reader_tests.exe') @(
        Compile 'tests\live_path_reader_tests.cpp'; Compile 'native\modules\common\il2cpp_runtime.cpp')
    Link (Join-Path $Tests 'live_range_reader_tests.exe') @(
        Compile 'tests\live_range_reader_tests.cpp'; Compile 'native\modules\common\il2cpp_runtime.cpp')
    Link (Join-Path $Tests 'enemy_definition_tests.exe') @(
        Compile 'tests\enemy_definition_tests.cpp'; Compile 'native\modules\common\il2cpp_runtime.cpp')
    Link (Join-Path $Tests 'features_display_tests.exe') @(
        Compile 'tests\features_display_tests.cpp'; Compile 'src\features\features.cpp')
    Link (Join-Path $Tests 'gfx_line_style_tests.exe') @(
        Compile 'tests\gfx_line_style_tests.cpp'; Compile 'src\gfx\gfx.cpp') @('-ld2d1','-ldwrite','-lgdi32','-luser32') -Wide
    Link (Join-Path $Tests 'http_tests.exe') @(
        Compile 'tests\http_tests.cpp'; Compile 'src\net\http.cpp') @('-lwinhttp', '-lws2_32')
    Link (Join-Path $Tests 'game_launch_request_tests.exe') @(
        Compile 'tests\game_launch_request_tests.cpp'; Compile 'src\shell\win_util.cpp';
        Compile 'src\log\log.cpp') @('-luser32', '-lgdi32', '-lshell32', '-lole32', '-luuid',
            '-lwindowscodecs', '-lmsimg32', '-ldwmapi', '-lshlwapi', '-ladvapi32') -Wide
    Link (Join-Path $Tests 'startup_setup_tests.exe') @(
        Compile 'tests\startup_setup_tests.cpp'; Compile 'src\game\startup_setup.cpp';
        Compile 'src\log\log.cpp'; Compile 'src\shell\win_util.cpp') @(
            '-luser32', '-lgdi32', '-lshell32', '-lole32', '-loleaut32', '-luuid',
            '-lwindowscodecs', '-lmsimg32', '-ldwmapi', '-lshlwapi') -Wide
    $loaderProbeObjects = @(Compile-All @('native\loader\win32_utils.cpp',
        'native\loader\process_watch.cpp', 'native\loader\target.cpp',
        'native\loader\console.cpp', 'native\loader\load_report.cpp'))
    foreach ($probe in @('remote_export_resolution_tests', 'window_thread_selection_tests')) {
        Link (Join-Path $Tests ($probe + '.exe')) (@(Compile ("tests\" + $probe + '.cpp')) +
            $loaderProbeObjects) @('-luser32', '-ladvapi32', '-lshell32') -Wide
    }
    Link (Join-Path $Tests 'statistics_persistence_tests.exe') @(
        Compile 'tests\statistics_persistence_tests.cpp'; Compile 'src\config\config.cpp';
        Compile 'native\modules\damage_meter\csv_export.cpp') -Wide
    Link (Join-Path $Tests 'risk_notice_tests.exe') @(
        Compile 'tests\risk_notice_tests.cpp'; Compile 'src\config\config.cpp')
    Link (Join-Path $Tests 'stage_overview_tests.exe') @(Compile 'tests\stage_overview_tests.cpp'; Compile 'src\ipc\ipc.cpp')
    Link (Join-Path $Tests 'dynamic_data_tests.exe') @(
        Compile 'tests\dynamic_data_tests.cpp'; Compile 'native\modules\provider\app\app.cpp')
    Link (Join-Path $Tests 'prts_parse_selftest.exe') @(Compile 'tests\prts\prts_parse_selftest.cpp'; Compile 'src\net\prts.cpp'; Compile 'src\net\prts_cache.cpp')
    Run-Test 'damage_meter_tests'
    Run-Test 'lifecycle_tests' @($Tests)
    Run-Test 'native_hook_tests' @($Tests)
    Run-Test 'builtin_module_tests'
    Run-Test 'production_statistics_tests'
    Run-Test 'ipc_tests'
    Run-Test 'feature_command_gate_tests'
    Run-Test 'enemy_publication_tests'
    Run-Test 'motion_predictor_tests'
    Run-Test 'overlay_range_tests'
    Run-Test 'overlay_window_geometry_tests'
    Run-Test 'range_projection_tests'
    Run-Test 'panel_session_tests'
    Run-Test 'battle_clock_tests'
    Run-Test 'menu_viewport_tests'
    Run-Test 'prts_client_tests'
    Run-Test 'prts_cache_tests'
    Run-Test 'path_animation_tests'
    Run-Test 'path_capture_tests'
    Run-Test 'operator_capture_tests'
    Run-Test 'autochess_statistics_tests'
    Run-Test 'live_path_reader_tests'
    Run-Test 'live_range_reader_tests'
    Run-Test 'enemy_definition_tests'
    Run-Test 'features_display_tests'
    Run-Test 'gfx_line_style_tests'
    Run-Test 'http_tests'
    Run-Test 'game_launch_request_tests'
    Run-Test 'startup_setup_tests'
    Run-Test 'remote_export_resolution_tests'
    Run-Test 'window_thread_selection_tests'
    Run-Test 'statistics_persistence_tests'
    Run-Test 'risk_notice_tests'
    Run-Test 'dynamic_data_tests'
    Run-Test 'stage_overview_tests'
    Run-Test 'prts_parse_selftest' @((Join-Path $Root 'tests\prts\prts'))
}
if ($Target -in @('All', 'Runtime')) {
    $runtimeSources = $Core + @(Get-Sources 'native\modules' -Recurse) + @('src\ipc\ipc.cpp')
    Link (Join-Path $Runtime 'ArkHelperRuntime.dll') (@(Compile-All $runtimeSources) + $MinHookObjects) @('-luser32') -Dll
    Link (Join-Path $Runtime 'ArkHelperBootstrap.dll') @(Compile 'native\bootstrap\message_hook.c') @('-luser32') -Dll
    Link (Join-Path $Runtime 'ArkHelperRuntimeHost.exe') @(Compile 'tests\runtime_host.cpp'; Compile 'src\ipc\ipc.cpp') @('-luser32') -Wide
}
if ($Target -in @('All', 'App')) {
    $iconResource = Join-Path $Objects 'arkhelp.res'
    Invoke-Zig @('rc', '/fo', $iconResource, '/i', (Join-Path $Root 'resources'), (Join-Path $Root 'resources\arkhelp.rc'))
    $appSources = @(Get-Sources 'src' -Recurse | Where-Object { $_ -notmatch '\\launcher\\' })
    $launcherSources = @(Get-Sources 'src\launcher' -Recurse) + @(Get-Sources 'native\loader')
    Link (Join-Path $Dist 'ArkHelper.exe') (@(Compile-All $appSources) + @($iconResource)) @('-luser32', '-lgdi32', '-lshell32',
        '-ladvapi32', '-lole32', '-loleaut32', '-luuid', '-ld2d1', '-ldwrite', '-ldwmapi', '-lshlwapi', '-lcomctl32',
        '-lwinhttp', '-lwindowscodecs', '-lmsimg32', '-lcomdlg32') -Gui
    Link (Join-Path $Dist 'ArkHelperLauncher.exe') (@(Compile-All $launcherSources) + @($iconResource)) @('-luser32', '-lshell32', '-ladvapi32') -Gui
    New-Item -ItemType Directory -Path (Join-Path $Dist 'data') -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $Root 'data') -File | Copy-Item -Destination (Join-Path $Dist 'data') -Force
    if (Test-Path -LiteralPath (Join-Path $Root 'packaging')) { Copy-Contents (Join-Path $Root 'packaging') $Dist }
}
Write-Host "ArkHelper $Target build complete: $Dist"
