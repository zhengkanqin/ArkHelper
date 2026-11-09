# ArkHelper

明日方舟 Windows x64 桌面信息辅助工具。

## 功能

- 战斗伤害、治疗、回费等统计与 CSV 导出。
- 我方与敌方数值、敌人血量、攻击范围和剩余路径可视化。
- 整关敌人资料、出怪列表与红门倒计时。
- PRTS 资料和头像查询，支持有上限的本地缓存。
- 可调整的悬浮球、菜单、独立面板、鼠标同步操作与显示样式。

## 使用

1. 从 [Releases](https://github.com/zhengkanqin/ArkHelper/releases/latest) 下载 Windows x64 程序包，完整解压到可写目录。
2. 打开 `ArkHelper.exe`，阅读并确认首次说明，再完成一次 Windows 管理员确认。助手会配置后台组件、识别游戏位置并创建桌面快捷方式。
3. 后续直接打开助手即可。无法识别游戏时，在设置中选择 `Arknights.exe`；同处可重新创建快捷方式、修复启动环境。移除后台组件可运行 `Uninstall-agent.cmd`。

## 项目承诺与使用风险

**本项目仅提供信息采集、统计、查询与数据可视化。当前和后续版本均不修改游戏玩法数据，不提供侵入式暴力功能。**

不提供修改生命、伤害、费用、资源或结算，自选/解锁干员，自定义关卡/藏品，绕过条件选关，无敌、无限资源、自动战斗或外部游戏插件加载。

**当前通过 DLL 注入和原生 Hook 采集信息。** 加载与 Hook 会写入启动代码和函数入口；“不改玩法数据”不表示游戏进程内存完全只读。使用可能造成检测、账号处罚（包括封禁）、崩溃或性能问题。本项目为非官方工具，不保证官方认可或账号安全。

关闭助手会停止采集；已加载的 DLL 随游戏退出释放。

## 构建

需要 Windows、PowerShell 7 与 Zig 0.15.1。将 Zig 加入 PATH，或放在 `.tools/zig/ziglang/zig.exe`。

```powershell
pwsh ./build.ps1       # 构建并执行回归，输出 dist/
pwsh ./verify.ps1      # 使用隔离测试宿主验证加载与启停
pwsh ./package.ps1     # Release 构建、验证及程序/源码打包
```

## 许可与来源

代码按 **GPL-3.0-or-later** 发布，见 [LICENSE](LICENSE)。原生加载、Hook 生命周期、任务调度、IL2CPP 读取及伤害统计部分改编自 ArkFramework，保留原作者与贡献者的权利。本项目已内化桌面界面、运行时、动态关卡读取、统计、缓存与可视化，移除了玩法写入接口、外部游戏插件加载及未使用的框架功能，无需单独安装 ArkFramework。

- 干员中文名辅助表来自 [ArknightsGameData](https://github.com/Kengxxiao/ArknightsGameData)。游戏数据与素材归其权利人所有，不分发完整关卡库、游戏客户端或程序集。
- 敌人资料和头像按需查询 [PRTS Wiki](https://prts.wiki/)，其贡献者内容遵循 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh)。`tests/prts/prts/source-*.wikitext` 是于 2026-11-07 录制的[源石虫](https://prts.wiki/w/源石虫)、[士兵](https://prts.wiki/w/士兵)、[霜星](https://prts.wiki/w/霜星)页面样本，其余两个样本为合成测试数据。
- MinHook 源码与许可见 `third_party/minhook-1.3.4`；LLVM 运行库、MinGW 与 Zig 许可见 `third_party/toolchain-licenses`。程序包附带适用许可。

再分发程序时须同时提供同版本完整对应源码、构建脚本及适用许可。

上述承诺约束本项目的功能设计，不额外限制 GPL 接收者的使用、修改和再分发权利。PRTS 内容和游戏相关素材分别遵循其权利人的许可。
