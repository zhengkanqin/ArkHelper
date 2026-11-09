ArkHelper 1.1.0
===============
Windows x64 明日方舟信息辅助工具。

使用：
1. 完整解压到可写目录，保留所有文件。
2. 首次连接游戏前运行 Install-agent.cmd，完成一次管理员授权。
3. 启动 ArkHelper.exe，阅读首次说明并确认风险，通过悬浮球或托盘管理功能。
Create-shortcut.cmd 创建快捷方式；Uninstall-agent.cmd 卸载后台组件。

功能：战斗统计与 CSV、双方数值、血量、攻击范围、剩余路径、整关敌人及出怪资料、PRTS 查询与缓存。
部分特殊模式的信息可能不可用；未获得的数据不以参考值替代。
四类面板可在各自设置中开启窗口点击穿透；开启后请从悬浮球菜单关闭，恢复窗口操作。

项目承诺：只做信息采集、统计、查询和可视化，不修改游戏玩法数据，不提供自选干员、自定义关卡/藏品、绕过条件选关、无敌、无限资源、自动战斗或外部插件加载。

技术与风险：仍使用 DLL 注入和原生 Hook。加载与函数入口会发生内存写入；可能触发检测、账号处罚（包括封禁）、崩溃或性能问题。不保证官方认可或账号安全。
首次说明须连续前台阅读超过 10 秒，主动勾选风险知晓并保存后才启用连接；关闭或不同意即退出。

配置：config/arkhelp.json
日志：logs/、runtime/logs/
战斗导出：runtime/exports/
PRTS 缓存默认 cache/prts/，可在设置中更改路径和上限。

代码 GPL-3.0-or-later；来源与修改见 licenses/Data-sources.md，许可全文见 LICENSE、licenses/。
项目：https://github.com/zhengkanqin/ArkHelper
程序与对应源码：https://github.com/zhengkanqin/ArkHelper/releases
