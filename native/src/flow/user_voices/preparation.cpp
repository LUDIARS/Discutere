#include "flow/user_voices/preparation.hpp"

#include <exception>

#include "base/time.hpp"
#include "flow/db/connection.hpp"
#include "flow/user_voices/request_client.hpp"

namespace di::flow::user_voices {
namespace {

/** error instanceof VoiceRequestFailure ? error.kind : "protocol" */
std::string failure_kind(const std::exception_ptr& e, bool* delivery_unknown) {
    try {
        std::rethrow_exception(e);
    } catch (const VoiceRequestFailure& f) {
        if (delivery_unknown) *delivery_unknown = f.delivery_unknown();
        return f.kind();
    } catch (...) {
        if (delivery_unknown) *delivery_unknown = true;  // VoiceRequestFailure でない失敗は結果不明扱い
        return "protocol";
    }
}

} // namespace

Json to_json(const VoicePreparation& s) {
    Json o = Json::object();
    o["referenceCount"] = s.referenceCount;
    o["priorKnowledge"] = s.priorKnowledge;
    o["requestId"] = s.requestId;
    o["choice"] = s.choice;
    o["requestStatus"] = s.requestStatus;
    if (s.failure) o["failure"] = *s.failure;
    return o;
}

VoicePreparation voice_preparation_from_json(const Json& j) {
    VoicePreparation s;
    s.referenceCount = json_number(j, "referenceCount");
    s.priorKnowledge = json_bool(j, "priorKnowledge");
    s.requestId = json_string(j, "requestId");
    s.requestStatus = json_string(j, "requestStatus");
    s.choice = json_string(j, "choice");
    s.failure = json_opt_string(j, "failure");
    return s;
}

std::optional<VoicePreparation> read_voice_preparation(const std::string& session_id) {
    auto row = db::get_flow_db()->get("SELECT payload FROM flow_voice_preparation WHERE session_id = ?", {session_id});
    if (!row) return std::nullopt;
    std::string error;
    auto j = parse_json(row->str("payload"), JsonLimits{kVoicePreparationMaxBytes, 1000, 8}, &error);
    if (!j || !j->is_object()) throw std::runtime_error("flow_voice_preparation.payload を読めません: " + error);
    return voice_preparation_from_json(*j);
}

void save_voice_preparation(const std::string& session_id, const VoicePreparation& state) {
    db::get_flow_db()->run(
        "INSERT INTO flow_voice_preparation (session_id, payload, updated_at) VALUES (?, ?, ?) ON CONFLICT(session_id) DO UPDATE SET "
        "payload=excluded.payload, updated_at=excluded.updated_at",
        {session_id, json_stringify(to_json(state)), static_cast<std::int64_t>(time::now_ms())});
}

VoicePreparation prepare_voice_choice(const PrepareVoiceChoiceArgs& args) {
    if (auto existing = read_voice_preparation(args.sessionId)) return *existing;  // 提出結果不明・再起動でも同じ依頼を再送しない。
    VoicePreparation state;
    state.referenceCount = args.referenceCount;
    state.priorKnowledge = args.priorKnowledge;
    state.requestId = "di:" + args.sessionId;
    state.choice = "unselected";
    state.requestStatus = args.referenceCount > 0 || args.priorKnowledge ? "not_needed" : args.request ? "submitting" : "unavailable";
    save_voice_preparation(args.sessionId, state);
    if (state.requestStatus != "submitting" || !args.request || !args.request->submit) return state;
    try {
        const auto receipt = args.request->submit(state.requestId, args.theme);
        state.requestId = receipt.requestId;
        state.requestStatus = receipt.accepted ? "accepted" : "submitted";
    } catch (...) {
        const auto e = std::current_exception();
        bool delivery_unknown = false;
        state.failure = failure_kind(e, &delivery_unknown);
        state.requestStatus = !delivery_unknown ? "unavailable" : "unknown";
    }
    save_voice_preparation(args.sessionId, state);
    return state;
}

bool needs_voice_choice(const std::optional<VoicePreparation>& state) {
    return state && state->requestStatus != "not_needed" && state->choice != "continue";
}

std::string voice_preparation_notice(const VoicePreparation& state) {
    const std::string reference = "外部の声は参考値です。なくても議論でき、後から意見・内容を修正して「再議論」できます。";
    if (state.requestStatus == "not_needed")
        return reference + "\n参考: 外部の声 " + js_number_to_string(state.referenceCount) + " 件" +
               (state.priorKnowledge ? "、事前知識・提示資料あり（実ユーザーの声とは別）" : "") + "。";
    const std::string status = state.requestStatus == "accepted"    ? "Voluptas の依頼受付を確認しました。感想の取得完了とは別です。"
                               : state.requestStatus == "submitted" ? "Voluptas に依頼を送信しました。受付はまだ確認できていません。"
                               : state.requestStatus == "unavailable"
                                   ? "Voluptas の依頼窓口が未設定のため、依頼は送信していません。"
                                   : "Voluptas への依頼結果が未確認です。重複送信せず受付を照合します。";
    const std::string f = state.failure.value_or("");
    const std::string failure = f == "down"            ? "Voluptas（Vo）が停止中、または接続できません。"
                                : f == "auth"          ? "Voluptas の依頼権限を確認できません。"
                                : f == "configuration" ? "Voluptas の接続設定を確認できません。"
                                : f == "protocol"      ? "Voluptas の受付応答を確認できません。"
                                                       : "";
    const std::string receipt =
        state.requestStatus == "unavailable" && state.failure && !state.failure->empty() ? "依頼の受付は確認できていません。" : status;
    return reference + "\n" + failure + receipt +
           "\n**「受付を待つ」** または **「外部の声なしで開始」** と回答してください。待機中は「受付確認」で状況を確認できます。";
}

std::optional<VoicePreparation> check_voice_receipt(const std::string& session_id, const std::optional<VoiceRequestPort>& request) {
    auto state = read_voice_preparation(session_id);
    if (!state || !request || !request->status || state->requestStatus == "not_needed") return state;
    try {
        state->requestStatus = request->status(state->requestId).accepted ? "accepted" : "submitted";
        state->failure.reset();
    } catch (...) {
        state->failure = failure_kind(std::current_exception(), nullptr);
    }
    save_voice_preparation(session_id, *state);
    return state;
}

} // namespace di::flow::user_voices
