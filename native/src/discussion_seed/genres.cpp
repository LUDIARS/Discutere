#include "discussion_seed/genres.hpp"

#include <stdexcept>

namespace di::discussion_seed {

Json to_json(const SeedTopic& t) {
    Json o = Json::object();
    o["title"] = t.title;
    o["description"] = t.description;
    return o;
}

const std::vector<GenreSeed>& GENRE_SEEDS() {
    static const std::vector<GenreSeed> seeds = {
        {"アクション", "操作の手応えと上達の実感"},
        {"シューティング", "弾幕と回避のリスク・リワード"},
        {"RPG", "成長と物語への没入"},
        {"シミュレーション", "システムを最適化する快感"},
        {"ストラテジー", "先読みと意思決定の重み"},
        {"アドベンチャー", "探索と発見の驚き"},
        {"パズル", "「分かった!」の瞬間の設計"},
        {"ローグライク", "ランダム性と一回性のスリル"},
        {"サバイバル", "リソース管理と緊張の持続"},
        {"対戦格闘", "読み合いと習熟の深さ"},
        {"スポーツ", "再現性と一瞬の判断"},
        {"レース", "速度感とライン取りの妙"},
        {"リズム", "音楽との一体感とフロー"},
        {"クラフト・サンドボックス", "自由な創造と所有の喜び"},
        {"ホラー", "恐怖の演出と緊張の緩急"},
        {"ノベル・アドベンチャー", "選択と物語の重み"},
        {"デッキ構築", "シナジーを組み上げる戦略性"},
    };
    return seeds;
}

SeedTopic build_genre_topic(const GenreSeed& seed) {
    const std::string angle = seed.angle && !seed.angle->empty() ? "特に「" + *seed.angle + "」の観点から、" : "";
    return {"【" + seed.genre + "】何が面白い?",
            "「" + seed.genre + "」というジャンルの面白さの核は何か。" + angle +
                "プレイヤーが夢中になる構造的な理由を、 複数の視点から掘り下げて議論する。"};
}

const GenreSeed& pick_genre_seed(std::int64_t index, const std::vector<GenreSeed>& seeds) {
    if (seeds.empty()) throw std::invalid_argument("genre seed list is empty");
    const auto n = static_cast<std::int64_t>(seeds.size());
    return seeds[static_cast<std::size_t>(((index % n) + n) % n)];
}

} // namespace di::discussion_seed
