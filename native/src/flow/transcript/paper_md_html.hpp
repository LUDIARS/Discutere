#pragma once
// ディスカッションペーパー本文 (markdown) を HTML にする。議論画面 (web/flow/page.html の renderPaperMd) と同じ規則:
// 「# 」→ h3、「## 」→ h4、「- 」「* 」→ 箇条書き、**太字**、それ以外は段落。他の記法は文字のまま。
// 文字はすべて HTML エスケープしてから変換する (本文に HTML が混じっても実行されない)。
#include <string>
#include <string_view>

namespace di::flow::transcript {

std::string render_paper_md_html(std::string_view md);

} // namespace di::flow::transcript
