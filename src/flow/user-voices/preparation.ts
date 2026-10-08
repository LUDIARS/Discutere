/** 議論ごとの参考情報と、Voluptas 受付待ちの明示的な選択。声の不足は開始を禁止しない。 */
import { getFlowDb } from "../db/connection.js";
import { VoiceRequestFailure } from "./request-client.js";

export interface VoiceRequestPort {
  submit(input: { requestId: string; theme: string }): Promise<{ accepted: boolean; requestId: string }>;
  status(requestId: string): Promise<{ accepted: boolean }>;
}

export interface VoicePreparation {
  referenceCount: number;
  priorKnowledge: boolean;
  requestId: string;
  requestStatus: "not_needed" | "submitting" | "submitted" | "accepted" | "unavailable" | "unknown";
  choice: "unselected" | "wait" | "continue";
  failure?: "down" | "auth" | "configuration" | "protocol";
}

export function readVoicePreparation(sessionId: string): VoicePreparation | null {
  const row = getFlowDb().prepare("SELECT payload FROM flow_voice_preparation WHERE session_id = ?").get(sessionId) as { payload: string } | undefined;
  return row ? JSON.parse(row.payload) as VoicePreparation : null;
}

export function saveVoicePreparation(sessionId: string, state: VoicePreparation): void {
  getFlowDb().prepare("INSERT INTO flow_voice_preparation (session_id, payload, updated_at) VALUES (?, ?, ?) ON CONFLICT(session_id) DO UPDATE SET payload=excluded.payload, updated_at=excluded.updated_at").run(sessionId, JSON.stringify(state), Date.now());
}

export async function prepareVoiceChoice(args: {
  sessionId: string; theme: string; referenceCount: number; priorKnowledge: boolean; request?: VoiceRequestPort;
}): Promise<VoicePreparation> {
  const existing = readVoicePreparation(args.sessionId);
  if (existing) return existing; // 提出結果不明・再起動でも同じ依頼を再送しない。
  const state: VoicePreparation = {
    referenceCount: args.referenceCount, priorKnowledge: args.priorKnowledge,
    requestId: `di:${args.sessionId}`, choice: "unselected",
    requestStatus: args.referenceCount > 0 || args.priorKnowledge ? "not_needed" : args.request ? "submitting" : "unavailable",
  };
  saveVoicePreparation(args.sessionId, state);
  if (state.requestStatus !== "submitting" || !args.request) return state;
  try {
    const receipt = await args.request.submit({ requestId: state.requestId, theme: args.theme });
    state.requestId = receipt.requestId;
    state.requestStatus = receipt.accepted ? "accepted" : "submitted";
  } catch (error) {
    state.failure = error instanceof VoiceRequestFailure ? error.kind : "protocol";
    state.requestStatus = error instanceof VoiceRequestFailure && !error.deliveryUnknown ? "unavailable" : "unknown";
  }
  saveVoicePreparation(args.sessionId, state);
  return state;
}

export function needsVoiceChoice(state: VoicePreparation | null): boolean {
  return !!state && state.requestStatus !== "not_needed" && state.choice !== "continue";
}

export function voicePreparationNotice(state: VoicePreparation): string {
  const reference = "外部の声は参考値です。なくても議論でき、後から意見・内容を修正して「再議論」できます。";
  if (state.requestStatus === "not_needed") return `${reference}\n参考: 外部の声 ${state.referenceCount} 件${state.priorKnowledge ? "、事前知識・提示資料あり（実ユーザーの声とは別）" : ""}。`;
  const status = state.requestStatus === "accepted" ? "Voluptas の依頼受付を確認しました。感想の取得完了とは別です。"
    : state.requestStatus === "submitted" ? "Voluptas に依頼を送信しました。受付はまだ確認できていません。"
    : state.requestStatus === "unavailable" ? "Voluptas の依頼窓口が未設定のため、依頼は送信していません。"
    : "Voluptas への依頼結果が未確認です。重複送信せず受付を照合します。";
  const failure = state.failure === "down" ? "Voluptas（Vo）が停止中、または接続できません。"
    : state.failure === "auth" ? "Voluptas の依頼権限を確認できません。"
    : state.failure === "configuration" ? "Voluptas の接続設定を確認できません。"
    : state.failure === "protocol" ? "Voluptas の受付応答を確認できません。" : "";
  const receipt = state.requestStatus === "unavailable" && state.failure ? "依頼の受付は確認できていません。" : status;
  return `${reference}\n${failure}${receipt}\n**「受付を待つ」** または **「外部の声なしで開始」** と回答してください。待機中は「受付確認」で状況を確認できます。`;
}

export async function checkVoiceReceipt(sessionId: string, request?: VoiceRequestPort): Promise<VoicePreparation | null> {
  const state = readVoicePreparation(sessionId);
  if (!state || !request || state.requestStatus === "not_needed") return state;
  try {
    state.requestStatus = (await request.status(state.requestId)).accepted ? "accepted" : "submitted";
    delete state.failure;
  } catch (error) { state.failure = error instanceof VoiceRequestFailure ? error.kind : "protocol"; }
  saveVoicePreparation(sessionId, state);
  return state;
}
