#pragma once
// src/flow/persona-pool.ts の移植: 匿名ペルソナプール + 嗜好近傍選定 (憑依/壁打ち相手の共通基盤)。
//
// - flow_persona: Voluptas 取込または公開レビュー話者の採用で得た永続ペルソナ。
// - select_by_affinity: ベクトル最近傍 (cosine) でペルソナを選ぶ = 「憑依」/壁打ち相手の選定。
//
// ベクトル空間は sentiment_vector の固定 20 次元 (新空間は作らない)。テキストからのベクトル化は text_to_vector を流用する。
// DB の JSON 列は外部入力として上限つきで読む (壊れていれば空とみなす。TS の parseJsonArray / parseJsonObject と同じ)。
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.hpp"
#include "flow/personas.hpp"

namespace di::flow {

/** "seed" | "adopted" | "imported" (旧データの "generated" 等もそのまま保持する)。 */
using PersonaOrigin = std::string;

struct PersonaAversion {
    std::string target;
    double strength = 0;
};

struct PersonaMechanicReaction {
    std::string mechanicId;
    double sentiment = 0;
};

struct PersonaAttributes {
    std::optional<std::string> ageBand;
    std::optional<std::string> spending;
};

struct PoolPersona {
    std::string id;
    std::string name;
    FlowRole role;
    std::string speechStyle;
    std::vector<std::string> traits;
    /** 固定 20 次元 affect ベクトル。 */
    std::vector<double> affectVector;
    PersonaOrigin origin;
    /** 旧データとの互換用。新規の合成生成は行わない。 */
    std::vector<std::string> parentIds;
    /** 由来した学習データセット名。 */
    std::optional<std::string> learningSource;
    std::optional<std::string> label;
    std::optional<std::string> model;
    /** C1 実在採用の話者アンカー `ext:<source>:<authorId>` (upsert キー)。 */
    std::optional<std::string> sourceSpeakerId;
    /** Voluptas が HMAC 仮名化した安定 user_id。生 SID は受理しない。 */
    std::optional<std::string> userId;
    /** 母集団平均からの近さ (cosine, 高い=典型)。 */
    std::optional<double> typicality;
    /** 極性の片寄り |pos-neg|/total (0=均衡 / 1=一方向)。 */
    std::optional<double> polarityBias;
    /** ゲーム間 affect のばらつき (per-game ベクトルの平均対距離)。 */
    std::optional<double> affectDispersion;
    /** 嗜好軸 → スコア (挿入順を保つ)。数値でない値は読み込み時に落とす。 */
    std::vector<std::pair<std::string, double>> preferenceAxes;
    PersonaAttributes attributes;
    std::vector<PersonaAversion> aversions;
    std::vector<PersonaMechanicReaction> mechanicReactions;
    std::optional<double> exportSpecVersion;
};

/** DB の JSON 列 (preference_axes_json 等) と同じ形。 */
Json preference_axes_to_json(const std::vector<std::pair<std::string, double>>& axes);
Json to_json(const PersonaAttributes& a);
Json to_json(const PersonaAversion& a);
Json to_json(const PersonaMechanicReaction& r);
/** TS の PoolPersona と同じキー順 (undefined のフィールドは出さない)。 */
Json to_json(const PoolPersona& p);

/** DB の 1 列 (JSON 文字列) の上限。 */
inline constexpr std::size_t kPersonaJsonColumnMaxBytes = 1u << 20;

/** ペルソナをプールに追加する (id は呼び出し側で採番)。affectVector が DIM 次元でなければ std::invalid_argument。 */
void insert_pool_persona(const PoolPersona& p);

/**
 * Voluptas の仮名 user_id で取込済みペルソナを引く。
 * include_archived はアーカイブ行も対象にする (unique index は archived を問わず効くので upsert の存在判定に要る)。
 */
std::optional<PoolPersona> find_pool_persona_by_user_id(const std::string& user_id, bool include_archived = false);

/** Voluptas 由来ペルソナを仮名 user_id 単位で冪等に取り込む (アーカイブ済みは復帰させる)。userId が無ければ std::invalid_argument。 */
PoolPersona upsert_pool_persona_by_user_id(const PoolPersona& persona);

/** source_speaker_id (ext:source:authorId) でプールペルソナを引く (C1 採用の upsert キー)。 */
std::optional<PoolPersona> find_pool_persona_by_speaker(const std::string& source_speaker_id);

/** 話者アンカー (source_speaker_id) を upsert する (C1)。sourceSpeakerId が無ければ std::invalid_argument。 */
PoolPersona upsert_pool_persona_by_speaker(const PoolPersona& p);

/** typicality を更新する (母集団平均確定後の再計算用)。 */
void set_pool_persona_typicality(const std::string& id, double typicality);

/** id でプールペルソナを取得する (非アーカイブのみ)。 */
std::optional<PoolPersona> get_pool_persona(const std::string& id);

struct PoolPersonaFilter {
    std::optional<PersonaOrigin> origin;
    std::optional<std::string> learningSource;
};

/** 非アーカイブのプールペルソナを列挙する (origin / learningSource で絞れる)。 */
std::vector<PoolPersona> list_pool_personas(const PoolPersonaFilter& filter = {});

/** id 完全一致 → name 完全一致 (trim 後) の順で探す (壁打ち相手指定)。非アーカイブのみ。 */
std::optional<PoolPersona> find_pool_persona(const std::string& id_or_name);

/** ペルソナをアーカイブする (論理削除)。 */
void archive_pool_persona(const std::string& id);

struct AffinityHit {
    PoolPersona persona;
    double similarity = 0;
};

/** ベクトル最近傍でペルソナを選ぶ (cosine 降順・安定)。candidates 省略時はプール全体から k 件。 */
std::vector<AffinityHit> select_by_affinity(const std::vector<double>& vector, int k = 1,
                                            const std::optional<std::vector<PoolPersona>>& candidates = std::nullopt);

/** テーマから嗜好/体験を類推して憑依ペルソナを選ぶ (B)。 */
std::vector<AffinityHit> select_possession_by_theme(const std::string& theme, int k = 1,
                                                    const std::optional<std::vector<PoolPersona>>& candidates = std::nullopt);

struct ToFlowPersonaOpts {
    std::optional<FlowRole> role;
    std::string defaultModel;
    bool isLocal = false;
    std::optional<FlowStance> stance;
};

/** プールペルソナを議論フローの FlowPersona に変換する (憑依/壁打ち相手として参加させる)。 */
FlowPersona to_flow_persona(const PoolPersona& p, const ToFlowPersonaOpts& opts);

/** 憑依対象ペルソナの「人物像」を prompt 用の短い説明文にする (B)。 */
std::string describe_possession(const PoolPersona& p);

} // namespace di::flow
