#pragma once
// src/flow/proxy/contract.ts の移植: 代理議論の人物パック契約 v1 (spec/interface/proxy-persona-pack.md)。
// 検証は入口で全部行い、通らない行は理由付きで拒否する:
//   - speakerKey は ext:<source>:<hmac16> の仮名だけ / 出典 URL・表示名・生の外部 ID に当たるフィールドは拒否
//   - 立場カードの evidenceIds は同じパックの evidence を指す / 20 次元ベクトルは sentiment-core v1 と同じ
// 判断は持たない。型と検証だけ (SRP)。エラーの文言と順序は TS と同じ。
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.hpp"

namespace di::flow::proxy {

inline constexpr int PROXY_CONTRACT_VERSION = 1;
inline constexpr std::size_t MAX_EVIDENCE = 12;
inline constexpr std::size_t MAX_EVIDENCE_CHARS = 400;
inline constexpr std::size_t MAX_STANCE_CARDS = 8;
inline constexpr std::size_t MAX_LIKED_GAMES = 32;

/** /^ext:[a-z][a-z0-9_-]*:[0-9a-f]{16}$/ */
bool is_speaker_key(std::string_view s);
/** /^[a-z0-9][a-z0-9_-]{0,63}$/ */
bool is_genre_id(std::string_view s);

struct PackSufficiency {
    double utterances = 0, positive = 0, negative = 0, games = 0, aspects = 0;
    std::string confidence;  // high | medium | low
    std::vector<std::string> reasons;
};

struct ConditionalGap {
    std::string current;
    std::string desired;
    std::optional<std::string> condition;
    std::vector<std::string> evidenceIds;
};

struct PackDesignGap {
    std::optional<std::vector<double>> vector;
    std::string method = "pos-minus-neg";
    std::vector<std::string> wants;
    std::vector<std::string> complaints;
    std::optional<std::string> reason;
};

struct StanceCard {
    std::string id;
    std::string claim;
    std::string polarity;  // pos | neg
    std::vector<std::string> evidenceIds;
    std::string extractor;  // rule | llm
};

struct PackEvidence {
    std::string id;
    std::string revision;
    std::string text;
    std::string sourceKind;
    std::string gameTitle;
    std::string polarity;
    std::optional<double> postedAt;
};

struct HeldOutLabel {
    std::string game;
    bool recommended = false;
};

struct PersonaPack {
    int contractVersion = PROXY_CONTRACT_VERSION;
    std::string personaKind = "observedSpeaker";
    std::string speakerKey;
    std::string genre;
    double genreSpecVersion = 0;
    std::string profileVersion;
    std::string formationMethod;  // formation.method
    std::string formationRuleVersion;
    std::optional<double> observedMonths;  // window.observedMonths
    double generatedAt = 0;
    double expiresAt = 0;
    PackSufficiency sufficiency;
    std::vector<double> affectVector;
    PackDesignGap designGap;
    std::vector<ConditionalGap> conditionalGaps;
    std::vector<StanceCard> stanceCards;
    std::vector<PackEvidence> evidence;
    std::vector<std::string> styleNotes;
    std::vector<std::string> likedGames;
    std::vector<HeldOutLabel> heldOut;
    bool revoked = false;
};

/** { ok: true, pack } | { ok: false, errors } */
struct PackValidation {
    bool ok = false;
    std::optional<PersonaPack> pack;
    std::vector<std::string> errors;
};

/** 人物パック 1 件を検証する。通れば正規化済みの PersonaPack を返す。 */
PackValidation parse_persona_pack(const Json& raw);

/** TS の PersonaPack と同じ形・同じキー順の JSON。 */
Json to_json(const PersonaPack& pack);
Json to_json(const PackValidation& v);

} // namespace di::flow::proxy
