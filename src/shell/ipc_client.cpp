#include "ipc_client.h"

#include "../log/log.h"
#include "platform.h"
#include "win_util.h"
#include <algorithm>
#include <array>

namespace arkhelp
{

bool CommandClient::open()
{
    if (view_ != nullptr)
    {
        return true;
    }

    // 创建失败时 GetLastError() 仍可能被后续调用覆盖，所以创建后立刻读取。
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(ipc::kCommandMappingBytes), ipc::kCommandMappingName);
    if (mapping_ == nullptr)
    {
        log_error("CreateFileMappingW(command) failed, code=%d", static_cast<int>(GetLastError()));
        return false;
    }
    const bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    view_ = static_cast<ipc::CommandState*>(
        MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::CommandState)));
    if (view_ == nullptr)
    {
        log_error("MapViewOfFile(command) failed, code=%d", static_cast<int>(GetLastError()));
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }

    const bool needs_init = !already_exists || view_->magic != ipc::kCommandMagic ||
                            view_->version != ipc::kVersion || view_->size != sizeof(ipc::CommandState);
    if (needs_init)
    {
        // 新映射（或 provider 上次运行留下的脏数据）必须重新初始化；
        // 用契约里的 begin_write/end_write 包住，避免 provider 读到半初始化的状态。
        uint32_t& sequence = view_->sequence;
        ipc::begin_write(sequence);
        view_->magic = ipc::kCommandMagic;
        view_->version = ipc::kVersion;
        view_->size = static_cast<uint32_t>(sizeof(ipc::CommandState));
        view_->feature_mask = 0;
        view_->card_index = 0;
        view_->revision = 1;
        view_->ui_scale = 1.0f;
        view_->flags = 0;
        view_->path_request_battle_serial=0;
        view_->path_request_count=0;
        std::fill(std::begin(view_->path_request_ids),std::end(view_->path_request_ids),0);
        ipc::end_write(sequence);
        log_info("command mapping initialized, name=%s, bytes=%d",
                 wide_to_utf8(ipc::kCommandMappingName).c_str(),
                 static_cast<int>(sizeof(ipc::CommandState)));
    }
    else
    {
        log_info("command mapping attached, feature_mask=%lu revision=%lu",
                 static_cast<unsigned long>(view_->feature_mask), static_cast<unsigned long>(view_->revision));
    }
    return true;
}

void CommandClient::close()
{
    if (view_ != nullptr)
    {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

uint32_t CommandClient::feature_mask() const
{
    return view_ != nullptr ? view_->feature_mask : 0u;
}

uint32_t CommandClient::revision() const
{
    return view_ != nullptr ? view_->revision : 0u;
}

uint32_t CommandClient::sequence() const
{
    return view_ != nullptr ? view_->sequence : 0u;
}

bool CommandClient::set_feature_mask(uint32_t mask)
{
    if (view_ == nullptr)
    {
        return false;
    }
    const auto flags = mask ? (view_->flags | ipc::kCommandOverlayEnabled)
                            : (view_->flags & ~ipc::kCommandOverlayEnabled);
    const bool clear_paths = !(mask & ipc::kFeatureEnemyPaths);
    const bool have_paths = view_->path_request_battle_serial || view_->path_request_count ||
        std::any_of(std::begin(view_->path_request_ids), std::end(view_->path_request_ids),
                    [](std::uint64_t id) { return id != 0; });
    if (view_->feature_mask == mask && view_->flags == flags && !(clear_paths && have_paths))
    {
        return false;
    }

    uint32_t& sequence = view_->sequence;
    ipc::begin_write(sequence);
    view_->feature_mask = mask;
    view_->flags = flags;
    if (clear_paths)
    {
        view_->path_request_battle_serial = 0;
        view_->path_request_count = 0;
        std::fill(std::begin(view_->path_request_ids), std::end(view_->path_request_ids), 0);
    }
    view_->revision = view_->revision + 1;
    ipc::end_write(sequence);

    log_info("feature_mask updated: 0x%lX revision=%lu", static_cast<unsigned long>(mask),
             static_cast<unsigned long>(view_->revision));
    return true;
}

bool CommandClient::set_feature_bit(ipc::Feature feature, bool enabled)
{
    if (view_ == nullptr)
    {
        return false;
    }
    const uint32_t bit = static_cast<uint32_t>(feature);
    const uint32_t mask = enabled ? (view_->feature_mask | bit) : (view_->feature_mask & ~bit);
    return set_feature_mask(mask);
}

bool CommandClient::set_ui_scale(float scale)
{
    if (view_ == nullptr)
    {
        return false;
    }
    float clamped = scale;
    if (clamped < 0.5f)
    {
        clamped = 0.5f;
    }
    if (clamped > 3.0f)
    {
        clamped = 3.0f;
    }
    if (view_->ui_scale == clamped)
    {
        return false;
    }

    uint32_t& sequence = view_->sequence;
    ipc::begin_write(sequence);
    view_->ui_scale = clamped;
    ipc::end_write(sequence);
    return true;
}

bool CommandClient::set_path_requests(std::uint32_t serial,std::span<const std::uint64_t> ids)
{
    if(!view_) return false;
    if (!ipc::command_feature_enabled(*view_, ipc::kFeatureEnemyPaths)) ids = {};
    std::array<std::uint64_t,ipc::kMaxPaths> requested{};
    std::uint32_t count=0;
    for(const auto id:ids) if(id && count<requested.size() &&
        std::find(requested.begin(),requested.begin()+count,id)==requested.begin()+count) requested[count++]=id;
    if(count==0) serial=0;
    if(view_->path_request_battle_serial==serial && view_->path_request_count==count &&
        std::equal(requested.begin(),requested.end(),std::begin(view_->path_request_ids))) return false;
    ipc::begin_write(view_->sequence);
    view_->path_request_battle_serial=serial;
    view_->path_request_count=count;
    std::copy(requested.begin(),requested.end(),std::begin(view_->path_request_ids));
    ipc::end_write(view_->sequence);
    return true;
}

bool BattleReader::open()
{
    if (view_ != nullptr)
    {
        return true;
    }

    // provider 缺席时 OpenFileMappingW 会失败，这是正常降级路径，不要记 ERROR。
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, ipc::kBattleMappingName);
    if (mapping == nullptr)
    {
        return false;
    }

    const void* mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (mapped == nullptr)
    {
        log_warn("MapViewOfFile(battle) failed, code=%d", static_cast<int>(GetLastError()));
        CloseHandle(mapping);
        return false;
    }

    const auto* snapshot = static_cast<const ipc::BattleSnapshot*>(mapped);
    if (snapshot->magic != ipc::kBattleMagic)
    {
        // 映射已建但 provider 尚未发布首批数据（magic 还是 0），下次轮询再试。
        UnmapViewOfFile(mapped);
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    view_ = snapshot;
    log_info("battle mapping attached, sequence=%lu", static_cast<unsigned long>(snapshot->sequence));
    return true;
}

void BattleReader::close()
{
    if (view_ != nullptr)
    {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool BattleReader::read(ipc::BattleSnapshot& out) const
{
    if (view_ == nullptr)
    {
        return false;
    }
    if (!ipc::read_snapshot(*view_, out))
    {
        return false;
    }
    return out.magic == ipc::kBattleMagic && out.version == ipc::kVersion &&
           out.size == static_cast<uint32_t>(sizeof(ipc::BattleSnapshot));
}

bool StageOverviewReader::read(ipc::StageOverview& out) {
    if (!view_) {
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, ipc::kOverviewMappingName);
        if (!mapping_) return false;
        view_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, ipc::kOverviewMappingBytes);
        if (!view_) { CloseHandle(mapping_); mapping_ = nullptr; return false; }
    }
    return ipc::read_overview(view_, out);
}
void StageOverviewReader::close() {
    if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
}

}  // namespace arkhelp
