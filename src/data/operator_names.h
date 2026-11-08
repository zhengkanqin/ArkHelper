#pragma once

#include <string>
#include <unordered_map>

// 干员 ID → 中文名映射。
//
// 战斗事件里只有 actor_id（例如 char_1035_wisdis），provider 侧目前不携带名字；
// 为了让伤害面板显示中文，外壳启动时读一次 <exe目录>/data/arkhelper-operators.tsv
// （由 ArkHelper/tools/gen_pack_v2.py 从 character_table.json 生成）。
// 文件缺失时不影响运行：调用方回退显示原始 ID。
namespace arkhelp
{

class OperatorNames
{
public:
    // 依次尝试 <exe目录>/data 与 <root>/data，读到多少算多少。
    void load(const std::wstring& exe_directory);

    // 未收录时返回空指针。
    const std::wstring* find(const char* actor_id) const;

    bool loaded() const { return !names_.empty(); }
    std::size_t size() const { return names_.size(); }

private:
    std::unordered_map<std::string, std::wstring> names_;
};

}  // namespace arkhelp
