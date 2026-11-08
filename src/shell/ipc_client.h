#pragma once

#include <arkhelp/ipc.h>
#include <arkhelp/stage_overview.h>

#include <cstdint>
#include <span>

#include "platform.h"

// 外壳对两个共享内存区的封装：
// - 命令区由 ArkHelper.exe 自己创建并写入（feature_mask / revision），provider 只读；
// - 战斗区由 provider 创建，外壳只读；provider 缺席时它可能始终不存在。
namespace arkhelp
{

class CommandClient
{
public:
    bool open();
    void close();
    bool ready() const
    {
        return view_ != nullptr;
    }

    uint32_t feature_mask() const;
    uint32_t revision() const;
    uint32_t sequence() const;

    // 同时同步采集启用标志；位掩码、标志或路径请求真正变化时 revision++ 并走 seqlock。
    bool set_feature_mask(uint32_t mask);
    bool set_feature_bit(ipc::Feature feature, bool enabled);
    // 把界面缩放同步给 provider（ipc.h 里的 ui_scale 字段），失败不影响主流程。
    bool set_ui_scale(float scale);
    bool set_path_requests(std::uint32_t serial,std::span<const std::uint64_t> ids);

private:
    HANDLE mapping_ = nullptr;
    ipc::CommandState* view_ = nullptr;
};

class BattleReader
{
public:
    // 打开失败返回 false（provider 还没发布），调用方可稍后重试。
    bool open();
    void close();
    bool is_open() const
    {
        return view_ != nullptr;
    }

    // 校验 magic/version/size 后经 ipc::read_snapshot 拷贝一份一致快照。
    bool read(ipc::BattleSnapshot& out) const;

private:
    HANDLE mapping_ = nullptr;
    const ipc::BattleSnapshot* view_ = nullptr;
};

class StageOverviewReader {
public:
    bool read(ipc::StageOverview& out);
    void close();
private:
    HANDLE mapping_ = nullptr;
    const void* view_ = nullptr;
};

}  // namespace arkhelp
