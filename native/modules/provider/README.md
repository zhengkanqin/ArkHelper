# 内置战斗数据模块

本目录静态链接到 ArkHelperRuntime.dll，由 native/modules/builtin.cpp 注册。

- capture：当前关卡、实例属性、战斗状态、单位采样、路径与主线程投影。
- data：复制的当前战斗数据模型，不读取离线关卡包。
- ipc：发布 IPC v8 与整关 StageOverview v1，处理显示查询及停止请求。
- app：只保留当前战斗；同关重开也清理旧状态。

界面和 PRTS 网络查询由桌面进程负责。运行时工作线程发布快照，Unity 数据读取在游戏主线程 Update Hook 中完成；运行时未就绪时待机重试。
