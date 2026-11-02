// lib/lapilli/packages/sentiment-core/data/{lexicon,vocabulary}.json をそのまま埋め込んだもの。
// 辞書を更新したら同じ内容に差し替える (tests/lib/sentiment_core_tests.cpp が元ファイルとの一致を確かめる)。
#include "lib/sentiment_core/lexicon.hpp"

namespace di::lib::sentiment_core {

std::string_view embedded_lexicon_json() {
    static constexpr std::string_view kText = R"DI_JSON({
  "_note": "日英の軽量辞書。polarity は単語→極性(-1..1)。emotions は Plutchik 8、aspects はゲーム評価軸のキーワード。substring マッチ(JP はトークナイザ無しのため)。拡張前提のコンパクト版。",
  "polarity": {
    "amazing": 1, "awesome": 1, "great": 0.8, "good": 0.6, "love": 0.9, "loved": 0.9, "fun": 0.8, "enjoy": 0.7, "enjoyable": 0.7, "excellent": 0.9, "best": 0.9, "perfect": 1, "masterpiece": 1, "addictive": 0.7, "addicting": 0.7, "recommend": 0.7, "fantastic": 0.9, "wonderful": 0.9, "smooth": 0.5, "worth": 0.6, "beautiful": 0.7, "solid": 0.5, "incredible": 0.9, "gem": 0.7, "polished": 0.6,
    "bad": -0.7, "boring": -0.8, "hate": -0.9, "terrible": -0.9, "awful": -0.9, "worst": -1, "broken": -0.8, "buggy": -0.7, "bug": -0.4, "bugs": -0.5, "lag": -0.6, "laggy": -0.6, "crash": -0.7, "crashes": -0.7, "refund": -0.7, "refunded": -0.8, "disappointing": -0.8, "disappointment": -0.8, "repetitive": -0.5, "grindy": -0.4, "pay2win": -0.8, "p2w": -0.8, "scam": -1, "frustrating": -0.7, "unplayable": -0.9, "trash": -0.9, "garbage": -0.9, "overpriced": -0.7, "stutter": -0.5, "mediocre": -0.5,
    "面白い": 0.8, "面白": 0.8, "神ゲー": 1, "神ゲ": 1, "神": 0.6, "最高": 0.9, "楽しい": 0.8, "楽し": 0.7, "おすすめ": 0.7, "良い": 0.6, "良ゲー": 0.8, "良作": 0.8, "名作": 0.9, "中毒": 0.6, "感動": 0.8, "快適": 0.6, "好き": 0.6, "神作": 1, "傑作": 0.9, "やり込める": 0.6,
    "つまらない": -0.8, "つまん": -0.8, "クソゲー": -1, "クソ": -0.7, "最悪": -0.9, "退屈": -0.7, "重い": -0.5, "萎え": -0.6, "ひどい": -0.8, "酷い": -0.8, "単調": -0.5, "飽き": -0.5, "詐欺": -1, "返金": -0.6, "ストレス": -0.6, "理不尽": -0.6, "課金ゲー": -0.7, "がっかり": -0.7, "バグだらけ": -0.8, "微妙": -0.4
  },
  "emotions": {
    "joy": ["fun", "love", "happy", "joy", "enjoy", "amazing", "awesome", "楽しい", "嬉しい", "神ゲー", "最高", "面白"],
    "trust": ["reliable", "recommend", "trust", "solid", "worth", "安心", "信頼", "おすすめ", "安定"],
    "fear": ["scary", "afraid", "fear", "terrifying", "怖い", "恐怖", "ホラー", "ビビ"],
    "surprise": ["surprising", "wow", "unexpected", "amazing", "驚", "びっくり", "すごい", "まさか"],
    "sadness": ["sad", "cry", "depressing", "lonely", "悲しい", "泣", "切ない", "鬱"],
    "disgust": ["disgusting", "gross", "cringe", "不快", "萎え", "キモ", "気持ち悪"],
    "anger": ["angry", "rage", "furious", "frustrating", "annoying", "怒", "ムカ", "理不尽", "クソ", "腹立"],
    "anticipation": ["hype", "excited", "can't wait", "looking forward", "楽しみ", "期待", "次回", "アプデ待ち"]
  },
  "aspects": {
    "fun": ["fun", "enjoyable", "addictive", "addicting", "entertaining", "面白", "楽し", "中毒", "ハマ"],
    "difficulty": ["hard", "difficult", "easy", "challenge", "challenging", "難し", "簡単", "難易度", "理不尽", "歯ごたえ"],
    "content": ["content", "hours", "amount", "variety", "endgame", "量", "ボリューム", "やり込み", "コンテンツ", "ボリュ"],
    "price_value": ["price", "cheap", "worth", "expensive", "value", "free", "値段", "安い", "高い", "コスパ", "課金", "無料"],
    "performance": ["lag", "fps", "crash", "optimization", "optimized", "performance", "bug", "stutter", "重い", "軽い", "最適化", "バグ", "落ちる", "カクつ"],
    "story": ["story", "plot", "narrative", "lore", "characters", "ストーリー", "物語", "シナリオ", "キャラ", "世界観"],
    "graphics": ["graphics", "art", "visual", "visuals", "beautiful", "aesthetic", "グラフィック", "綺麗", "ビジュアル", "作画", "美麗"],
    "replayability": ["replay", "replayability", "again", "addictive", "やり込み", "繰り返し", "リプレイ", "周回", "リプレ"]
  },
  "arousal": ["!", "!!", "amazing", "incredible", "insane", "wow", "epic", "best", "worst", "神", "最高", "すごい", "やばい", "ヤバ", "神ゲー"]
}
)DI_JSON";
    return kText;
}

std::string_view embedded_vocabulary_json() {
    static constexpr std::string_view kText = R"DI_JSON({
  "_note": "Affect 統制語彙 (サンプル/Phase 0)。mechanics.intended_affect と 観測 affect (translation_proposals.target_name / affects.mood) が同じ `key` を完全一致で参照する。matcher の負感情ハードコード集合 (frustration/anger/sadness/boredom) と綴りを一致させてある。",
  "workspace_id": "knowledge",
  "version": "0.1.0",
  "valence_values": ["positive", "negative", "ambivalent"],
  "affects": [
    { "key": "accomplishment", "label_ja": "達成感", "valence": "positive", "mda": "Challenge", "description": "困難を乗り越えて目標を成し遂げた充足。" },
    { "key": "mastery", "label_ja": "上達感", "valence": "positive", "mda": "Challenge", "description": "繰り返しで腕が上がり自分の成長を実感する。" },
    { "key": "tension_release", "label_ja": "緊張と解放", "valence": "positive", "mda": "Challenge", "description": "張り詰めた局面を切り抜けた瞬間のカタルシス。" },
    { "key": "immersion", "label_ja": "没入", "valence": "positive", "mda": "Fantasy", "description": "世界・役割に入り込み現実を忘れる感覚。" },
    { "key": "discovery", "label_ja": "発見", "valence": "positive", "mda": "Discovery", "description": "未知の場所・仕組み・攻略を見つけた喜び。" },
    { "key": "flow", "label_ja": "フロー", "valence": "positive", "mda": "Submission", "description": "難易度と技量が釣り合い時間を忘れて没頭する状態。" },
    { "key": "game_feel", "label_ja": "爽快感", "valence": "positive", "mda": "Sensation", "description": "操作の手触り・打撃・反応の気持ちよさ (juiciness)。" },
    { "key": "excitement", "label_ja": "高揚", "valence": "positive", "mda": "Sensation", "description": "盛り上がり・テンションの上昇。" },
    { "key": "collection", "label_ja": "収集の充足", "valence": "positive", "mda": "Discovery", "description": "集める・埋める・コンプリートする満足。" },
    { "key": "expression", "label_ja": "自己表現", "valence": "positive", "mda": "Expression", "description": "ビルド・装飾・プレイスタイルで自分らしさを出す。" },
    { "key": "fellowship", "label_ja": "連帯", "valence": "positive", "mda": "Fellowship", "description": "協力・共有・コミュニティの一体感。" },
    { "key": "narrative", "label_ja": "物語的感動", "valence": "positive", "mda": "Narrative", "description": "ストーリー・キャラ・世界観に心を動かされる。" },
    { "key": "wonder", "label_ja": "驚き・畏怖", "valence": "positive", "mda": "Sensation", "description": "想定を超える光景や仕掛けへの驚嘆。" },
    { "key": "strategic", "label_ja": "戦略的満足", "valence": "positive", "mda": "Challenge", "description": "読み・組み立て・最適解が決まった知的快感。" },

    { "key": "frustration", "label_ja": "理不尽・ストレス", "valence": "negative", "description": "不公平・操作不能・ハメ等による苛立ち。matcher 負集合。" },
    { "key": "boredom", "label_ja": "退屈", "valence": "negative", "description": "刺激不足・進展のなさによる飽き。matcher 負集合。" },
    { "key": "monotony", "label_ja": "単調(作業感)", "valence": "negative", "description": "同じ作業の繰り返しによる疲弊 (grind)。" },
    { "key": "confusion", "label_ja": "混乱", "valence": "negative", "description": "目的・操作・情報が分からず迷う。" },
    { "key": "anger", "label_ja": "怒り", "valence": "negative", "description": "強い不満・憤り。matcher 負集合。" },
    { "key": "sadness", "label_ja": "喪失感", "valence": "negative", "description": "進捗・資産・愛着の喪失による落胆。matcher 負集合。" },
    { "key": "unfair_monetization", "label_ja": "課金不公平感", "valence": "negative", "description": "pay2win・搾取的課金への反発。" },

    { "key": "fear", "label_ja": "恐怖", "valence": "ambivalent", "description": "怖さ。ホラーでは意図された正、それ以外では負になりうる。" },
    { "key": "suspense", "label_ja": "緊迫", "valence": "ambivalent", "description": "結果が読めない張り詰め。演出として正、過剰だと負。" },
    { "key": "solitude", "label_ja": "孤独", "valence": "ambivalent", "description": "ひとりの感覚。探索美として正、疎外感だと負。" }
  ]
}
)DI_JSON";
    return kText;
}

} // namespace di::lib::sentiment_core
