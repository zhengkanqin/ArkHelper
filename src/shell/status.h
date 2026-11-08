#pragma once

#include <cstdint>
#include <string>

// 外壳侧对 provider 运行状态的统一视图。
// 悬浮球状态点、功能菜单状态行、日志都消费这一份数据，避免各处重复读共享内存。
namespace arkhelp
{

enum class LinkState
{
    kDisconnected = 0,  // 灰：战斗共享内存不存在 / magic 不匹配
    kWaiting = 1,       // 黄：provider 已就绪但当前不在战斗中
    kBattle = 2,        // 绿：正在战斗
};

struct Status
{
    LinkState link = LinkState::kDisconnected;

    bool battle_mapping_open = false;  // Local\ArkHelp.Battle.v1 是否可映射
    bool battle_valid = false;         // 最近一次 read_snapshot 是否成功且通过 magic 校验
    bool in_battle = false;
    int battle_state = -1;  // ipc.h: -1 未知，2 运行中，3 已结束
    double time_seconds = 0.0;
    std::string level_id;
    uint32_t sequence = 0;
    uint32_t frame = 0;

    bool command_mapping = false;  // Local\ArkHelp.Command.v1 是否可写
    uint32_t feature_mask = 0;
    uint32_t revision = 0;

    // 游戏客户端进程检测结果（例如 "Arknights.exe"；空表示未检测到）。
    std::wstring game_process;
    std::wstring launch_error;
};

inline const wchar_t* link_state_label(LinkState state)
{
    switch (state)
    {
    case LinkState::kBattle:
        return L"战斗中";
    case LinkState::kWaiting:
        return L"连接中";
    case LinkState::kDisconnected:
    default:
        return L"未连接";
    }
}

}  // namespace arkhelp
