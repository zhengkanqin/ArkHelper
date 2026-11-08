# 数据与组件来源

- 关卡、单位属性及局内统计来自本机游戏运行时；相关游戏内容归其权利人所有。
- 干员中文名辅助表来源于 [Kengxxiao/ArknightsGameData](https://github.com/Kengxxiao/ArknightsGameData) 国服数据。
- 敌人资料和头像按需查询 [PRTS Wiki](https://prts.wiki/)；其贡献者内容遵循 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh)。测试样本的来源另见 tests/prts/prts/README.md。
- 不分发完整关卡库、游戏客户端、游戏程序集或内存转储。
- ArkHelper 代码按 GPL-3.0-or-later 发布，原生加载、Hook 生命周期、任务调度、IL2CPP 读取及伤害统计部分改编自 ArkFramework。原作者和贡献者的权利保留，本项目不代表上游作者背书。
- MinHook 源码及许可位于 third_party/minhook-1.3.4；编译工具链的 LLVM 运行库、MinGW 与 Zig 许可位于 third_party/toolchain-licenses。相关许可随程序复制到 licenses/。

2026-11-09，ArkHelper 1.0.0 将桌面界面、动态关卡读取、统计、缓存与可视化内化为独立产品；删除未使用的托管写入/构造接口、旧输入拦截、设置窗口、重复 PRTS 客户端、离线关卡加载器、投影探测代码和 ImGui 依赖。正式运行时仅使用静态内置模块，外部模块加载与旧输入适配仅供隔离测试。

分发程序须同时提供同版本完整对应源码、构建脚本与适用许可。信息辅助承诺约束本项目的设计和维护方向，不额外限制 GPL 接收者的权利。代码许可不代替素材、游戏规则或账号使用方面的授权。
