#include "operator_names.h"

#include <cstdio>

#include "../log/log.h"
#include "../shell/win_util.h"

namespace arkhelp
{
namespace
{

bool read_text_file(const std::wstring& path, std::string& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER size{};
    bool ok = false;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 4 * 1024 * 1024)
    {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < out.size())
        {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(out.size() - offset);
            if (!ReadFile(file, out.data() + offset, chunk, &read, nullptr) || read == 0)
            {
                break;
            }
            offset += read;
        }
        out.resize(offset);
        ok = offset > 0;
    }
    CloseHandle(file);
    return ok;
}

void parse_lines(const std::string& text, std::unordered_map<std::string, std::wstring>& names)
{
    std::size_t position = 0;
    while (position < text.size())
    {
        std::size_t end = text.find('\n', position);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        std::string line = text.substr(position, end - position);
        position = end + 1;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
        {
            continue;
        }
        names.emplace(line.substr(0, tab), utf8_to_wide(line.substr(tab + 1)));
    }
}

}  // namespace

void OperatorNames::load(const std::wstring& exe_directory)
{
    names_.clear();
    const std::wstring candidates[] = {
        join_path(join_path(exe_directory, L"data"), L"arkhelper-operators.tsv"),
        join_path(join_path(app_root(), L"data"), L"arkhelper-operators.tsv"),
    };
    std::string text;
    for (const std::wstring& path : candidates)
    {
        if (read_text_file(path, text))
        {
            parse_lines(text, names_);
            log_info("operator names loaded: %zu entries from %s", names_.size(),
                     wide_to_utf8(path).c_str());
            return;
        }
    }
    log_warn("operator names missing (data/arkhelper-operators.tsv); falling back to raw ids");
}

const std::wstring* OperatorNames::find(const char* actor_id) const
{
    if (actor_id == nullptr || *actor_id == '\0')
    {
        return nullptr;
    }
    const auto found = names_.find(actor_id);
    return found == names_.end() ? nullptr : &found->second;
}

}  // namespace arkhelp
