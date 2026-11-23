#include "flow/dialectic/argument_pattern_catalog.hpp"

#include <mutex>
#include <stdexcept>

#include "base/fsutil.hpp"
#include "base/log.hpp"
#include "base/sidecar.hpp"
#include "base/strings.hpp"

namespace di::flow::dialectic {

namespace {

// data/argument-patterns.json と同じ内容 (変えるときは両方を直す。テストが一致を確かめる)。
constexpr std::string_view kEmbedded = R"JSON({
  "version": "di-2026-11-23.1",
  "source": "Histrio spec/feature/virtual-persona-life-script.md §2 (script_patterns) / §4.1 (揺れ先)。Histrio の script_patterns が動くまでの Di 側の版付きの写し",
  "scriptPatterns": [
    {
      "id": "until",
      "label": "〜するまでは",
      "position": "確度は「条件つきで」までに抑え、主張が成り立つための前提条件を例外条件に並べる。",
      "exchange": "相手の主張の前提条件がまだ揃っていない点 (限定の範囲・確度) を突く。",
      "stress": "押されている。「まだ決めるには早い」と保留に寄りたくなる。保留するなら、足りない前提が何かを 1 つ名指しする。",
      "stressQuadrant": "I-U+"
    },
    {
      "id": "after",
      "label": "〜した後で",
      "position": "うまくいった後に起きる揺り戻し (飽き・反動・副作用) を例外条件に挙げる。",
      "exchange": "決着しかけた論拠に、決まった後に起きるリスクを例外条件として足す。",
      "stress": "押されている。決まりかけたことを蒸し返したくなる。蒸し返すなら、後で起きるリスクの裏付けを 1 つ示す。",
      "stressQuadrant": "I-U+"
    },
    {
      "id": "never",
      "label": "決して",
      "position": "今のままを守る側に立ち、確度を強めに言い切る。",
      "exchange": "新しい案のデータが本当にその仕様の結果かを疑う。",
      "stress": "押されている。認めたくない。それでも崩せないと思う論拠は認める。認めない場合は、相手のデータのどこが疑わしいかを具体的に言う。",
      "stressQuadrant": "I+U-"
    },
    {
      "id": "always",
      "label": "いつも",
      "position": "前例 (他のゲームではこうなった) を主な論拠にし、裏付けも前例で埋める。",
      "exchange": "他のゲームの前例で押す。",
      "stress": "押されている。同じ前例を繰り返したくなる。繰り返すなら、その前例と今回の仕様が同じだと言える点を 1 つ示す。",
      "stressQuadrant": "I+U-"
    },
    {
      "id": "almost",
      "label": "もう少しで",
      "position": "根拠を厚くしようとし、確かめ切れていない点を 1 つ残す。",
      "exchange": "決着間際に、まだ検討していない点を 1 つ差し込む。",
      "stress": "押されている。「あと一点だけ」と決着を延ばしたくなる。延ばすなら、その一点がこの論点の答えを変えうる理由を言う。",
      "stressQuadrant": "I-U+"
    },
    {
      "id": "open_ended",
      "label": "結末のない",
      "position": "作りたい体験から逆算して (アブダクション) 主張し、範囲を広めに取る。",
      "exchange": "決着した後に「では次に何を確かめるか」を問う。",
      "stress": "押されている。結論を出さずに次の話へ移りたくなる。移る前に、この論点で残った答えを一言でまとめる。",
      "stressQuadrant": "I+U+"
    }
  ],
  "reasoningStyles": [
    {
      "id": "deduction",
      "label": "演繹",
      "line": "論じ方の好み: 演繹 (この仕様を入れて、こういう人が遊ぶと、こういう体験になる) で組み立てがち。"
    },
    {
      "id": "induction",
      "label": "帰納",
      "line": "論じ方の好み: 帰納 (この仕様を入れている他のゲームでは、こういう体験になっていた) で組み立てがち。"
    },
    {
      "id": "abduction",
      "label": "アブダクション",
      "line": "論じ方の好み: アブダクション (こういう体験を作りたいから、きっとこういう仕様が良い) で組み立てがち。"
    }
  ]
})JSON";

std::string required(const Json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string() || str::trim(it->get<std::string>()).empty())
        throw std::runtime_error(std::string("argument-patterns: ") + key + " がありません");
    return str::trim(it->get<std::string>());
}

} // namespace

ArgumentPatternCatalog parse_argument_pattern_catalog(const Json& j) {
    if (!j.is_object()) throw std::runtime_error("argument-patterns: オブジェクトではありません");
    ArgumentPatternCatalog c;
    c.version = required(j, "version");
    const auto sp = j.find("scriptPatterns");
    const auto rs = j.find("reasoningStyles");
    if (sp == j.end() || !sp->is_array() || sp->empty()) throw std::runtime_error("argument-patterns: scriptPatterns が空です");
    if (rs == j.end() || !rs->is_array() || rs->empty()) throw std::runtime_error("argument-patterns: reasoningStyles が空です");
    for (const auto& p : *sp)
        c.scriptPatterns.push_back({required(p, "id"), required(p, "label"), required(p, "position"), required(p, "exchange"),
                                    required(p, "stress"), required(p, "stressQuadrant")});
    for (const auto& r : *rs) c.reasoningStyles.push_back({required(r, "id"), required(r, "label"), required(r, "line")});
    return c;
}

std::string_view embedded_argument_pattern_catalog_json() { return kEmbedded; }

const ArgumentPatternCatalog& argument_pattern_catalog() {
    static std::once_flag once;
    static ArgumentPatternCatalog catalog;
    std::call_once(once, [] {
        const std::string path = fs::join(sidecar::repo_root(), "data/argument-patterns.json");
        if (fs::is_file(path)) {
            try {
                if (auto text = fs::read_file(path, 1u << 20)) {
                    catalog = parse_argument_pattern_catalog(parse_json_or_throw(*text));
                    return;
                }
            } catch (const std::exception& e) {
                log::warn(std::string("論述パターンのカタログを読めません (組み込み版を使う): ") + e.what());
            }
        }
        catalog = parse_argument_pattern_catalog(parse_json_or_throw(kEmbedded));
    });
    return catalog;
}

const ScriptPattern* find_script_pattern(const ArgumentPatternCatalog& c, std::string_view id_or_label) {
    const std::string key = str::trim(std::string(id_or_label));
    for (const auto& p : c.scriptPatterns)
        if (p.id == key || p.label == key) return &p;
    return nullptr;
}

const ReasoningStyle* find_reasoning_style(const ArgumentPatternCatalog& c, std::string_view id_or_label) {
    const std::string key = str::trim(std::string(id_or_label));
    for (const auto& r : c.reasoningStyles)
        if (r.id == key || r.label == key) return &r;
    return nullptr;
}

} // namespace di::flow::dialectic
