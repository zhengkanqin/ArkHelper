// Exercise the production PRTS parser and request worker with recorded pages.
#include "net/prts.h"
#include "net/http.h"
#include "shell/platform.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace {
std::map<std::wstring, std::string> pages;
int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
std::string quote(const std::string& text) {
    std::string result = "\"";
    for (unsigned char ch : text) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\r': result += "\\r"; break;
        case '\n': result += "\\n"; break;
        case '\t': result += "\\t"; break;
        default: result += static_cast<char>(ch); break;
        }
    }
    return result + "\"";
}
std::wstring field(const std::vector<arkhelp::net::PrtsField>& fields, const wchar_t* key) {
    for (const auto& value : fields) if (value.key == key) return value.value;
    return {};
}
arkhelp::net::PrtsResult read(arkhelp::net::PrtsClient& client, const wchar_t* name) {
    client.request(name);
    const auto deadline = GetTickCount64() + 2500;
    while (GetTickCount64() < deadline) {
        std::wstring returned;
        arkhelp::net::PrtsResult result;
        while (client.poll(returned, result)) if (returned == name) return result;
        Sleep(1);
    }
    throw std::runtime_error("recorded page request did not reach a terminal result");
}
}

namespace arkhelp::net {
std::string url_encode(const std::string& text) { return text; }
bool http_get(const std::wstring& url, std::string& body, std::string& error, const HttpOptions&) {
    for (const auto& [name, page] : pages) {
        if (url.find(L"&page=" + name) == std::wstring::npos) continue;
        body = "{\"parse\":{\"wikitext\":" + quote(page) + "}}";
        return true;
    }
    error = "fixture has no page for this request";
    return false;
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::puts("usage: prts_parse_selftest <sample-directory>"); return 2; }
    try {
        const std::filesystem::path directory = argv[1];
        for (const auto& [name, file] : std::map<std::wstring, const char*>{
            {L"源石虫", "source-slime.wikitext"}, {L"士兵", "source-soldier.wikitext"},
            {L"霜星", "source-frostnova.wikitext"}, {L"缺失", "missing-fields.wikitext"},
            {L"干员", "operator-page.wikitext"}}) {
            std::ifstream stream(directory / file, std::ios::binary);
            check(static_cast<bool>(stream), "recorded page exists");
            pages[name] = {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }
        arkhelp::net::PrtsClient client;
        const auto slime = read(client, L"源石虫");
        check(slime.ok, "slime parses");
        check(field(slime.basic, L"描述").find(L"被感染生物") != std::wstring::npos, "slime description");
        check(slime.levels.size() == 2, "slime includes both levels");
        check(field(slime.levels[0].fields, L"最大生命值") == L"550", "slime level zero health");
        check(field(slime.levels[0].fields, L"攻击力") == L"130", "slime attack");
        check(field(slime.levels[1].fields, L"最大生命值") == L"2050", "slime level one override");
        const auto soldier = read(client, L"士兵");
        check(soldier.ok && soldier.levels.size() == 2, "soldier levels parse");
        check(field(soldier.levels[0].fields, L"最大生命值") == L"1650", "soldier base health");
        check(field(soldier.levels[1].fields, L"最大生命值") == L"2750", "soldier health override");
        check(field(soldier.levels[1].fields, L"防御力") == L"130", "soldier defense override");
        const auto frost = read(client, L"霜星");
        check(frost.ok && !frost.levels.empty(), "boss page with nested markup parses");
        check(!field(frost.levels[0].fields, L"技能0").empty(), "boss skill remains available");
        check(field(frost.levels[0].fields, L"技能0效果").find(L"{{") == std::wstring::npos,
              "boss skill markup is cleaned by production parser");
        const auto missing = read(client, L"缺失");
        check(missing.ok && missing.levels.empty(), "missing optional level fields do not reject enemy page");
        const auto wrong = read(client, L"干员");
        check(!wrong.ok && !wrong.error.empty(), "non-enemy page produces explicit error");
        std::printf("PRTS production parser: %d checks, PASS\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL PRTS recorded page: %s\n", error.what()); return 1;
    }
}
