# ArkHelper

明日方舟 Windows x64 桌面信息辅助工具。

## 功能

- 战斗伤害、治疗、回费等统计与 CSV 导出。
- 我方与敌方数值、敌人血量、攻击范围和剩余路径可视化。
- 整关敌人资料、出怪列表与红门倒计时。
- PRTS 资料和头像查询，支持有上限的本地缓存。
- 可调整的悬浮球、菜单、独立面板、点击穿透与显示样式。

## 使用

1. 从 [Releases](https://github.com/zhengkanqin/ArkHelper/releases/latest) 下载 Windows x64 程序包，完整解压到可写目录。
2. 首次连接游戏前运行 `Install-agent.cmd`，完成一次管理员授权；移除后台组件可运行 `Uninstall-agent.cmd`。
3. 启动 `ArkHelper.exe`，阅读首次使用说明并确认风险，通过悬浮球或托盘选择功能。可运行 `Create-shortcut.cmd` 创建快捷方式。

说明窗须连续前台阅读**超过 10 秒**，主动勾选已知晓风险后才能确认；确认前不会自动启动游戏或连接采集。游戏运行时才绘制画面标记，战后数据面板保留供核对。部分特殊模式的信息可能不可用，缺失值显示“—”。

## 项目承诺与使用风险

**本项目仅提供信息采集、统计、查询与数据可视化。当前和后续版本均不修改游戏玩法数据，不提供侵入式暴力功能。**

不提供修改生命、伤害、费用、资源或结算，自选/解锁干员，自定义关卡/藏品，绕过条件选关，无敌、无限资源、自动战斗或外部游戏插件加载。

**当前通过 DLL 注入和原生 Hook 采集信息。** 加载与 Hook 会写入启动代码和函数入口；“不改玩法数据”不表示游戏进程内存完全只读。使用可能造成检测、账号处罚（包括封禁）、崩溃或性能问题。本项目为非官方工具，不保证官方认可或账号安全。

## 构建

需要 Windows、PowerShell 7 与 Zig 0.15.1。将 Zig 加入 PATH，或放在 `.tools/zig/ziglang/zig.exe`。

```powershell
pwsh ./build.ps1       # 构建并执行回归，输出 dist/
pwsh ./verify.ps1      # 使用隔离测试宿主验证加载与启停
pwsh ./package.ps1     # Release 构建、验证及程序/源码打包
```

详细说明：[架构](docs/internalization.md) · [数据边界](docs/runtime-data.md) · [验收清单](docs/manual-validation.md) · [Framework 关联审计](docs/framework-separation-audit.md)。

## 许可与来源

代码按 **GPL-3.0-or-later** 发布。部分底层代码改编自 ArkFramework；来源、修改及第三方许可见 [数据与组件来源](docs/data-sources.md) 和 [LICENSE](LICENSE)。发布程序须同时提供完整对应源码与构建脚本。

上述承诺约束本项目的功能设计，不额外限制 GPL 接收者的使用、修改和再分发权利。PRTS 内容和游戏相关素材分别遵循其权利人的许可。
