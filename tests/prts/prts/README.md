# PRTS 离线样本

这些样本通过模拟传输送入桌面端实际 PrtsClient，验证生产解析器，不访问网络。

| 文件 | 来源 |
| --- | --- |
| source-slime.wikitext | [源石虫](https://prts.wiki/w/源石虫) |
| source-soldier.wikitext | [士兵](https://prts.wiki/w/士兵) |
| source-frostnova.wikitext | [霜星](https://prts.wiki/w/霜星) |
| missing-fields.wikitext | 合成的缺字段敌人页面 |
| operator-page.wikitext | 合成的非敌人页面 |

真实样本抓取于 2026-11-07，内容属于 PRTS Wiki 及其贡献者，遵循 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh)。

在项目根运行 `pwsh ./build.ps1 -Target Tests`，其中 prts_parse_selftest 验证录制页面，prts_client_tests 验证 HTML/模板清理、请求队列、失败重试及缓存交互。
