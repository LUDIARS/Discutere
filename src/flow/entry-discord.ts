/**
 * Discord フォーラム投稿を入口とするフロー起動 (t7-entrypoints.md 経路 B)。
 *
 * フォーラムの **投稿が UI**。最初の投稿 (starter) をトリガーにテーマを受信し、
 * **議論タイプ (flow) とタグ (機密/内部/運用/開発) はいずれもフォーラムの適用タグから取得**する
 * (slash / ボタンは使わない、OVERVIEW §9 の決定)。議論タイプは必須 (未指定は受理しない)。
 *
 * 認証は既存どおり Discord 依存 (bot token + admin allowlist、Cernere 非使用、CLAUDE.md)。
 * 本モジュールは「フォーラム適用タグ → (flow, tags) → dispatch」の純パーサ + ハンドラのみを提供する。
 * gateway への配線 (ThreadCreate フック) は既存フォーラム経路を壊さないよう別途行う。
 */

import { parseFlowKind, dispatchFlow, type DispatchDeps, type DispatchResult, type FlowKind } from "./dispatch.js";
import type { FlowTag } from "./tags.js";

/** フォーラム適用タグ名 → FlowKind / FlowTag のマッピング (config 上書き可)。 */
export interface ForumTagMapping {
  /** 議論タイプ判定用のタグ名 (kind ごと)。parseFlowKind の別名と統合される。 */
  flowKindTagNames?: Partial<Record<FlowKind, string[]>>;
  /** 機密度+観点タグの名前 (既定: 機密/内部/運用/開発 のリテラル)。 */
  flowTagNames?: string[];
}

const DEFAULT_FLOW_TAG_NAMES: FlowTag[] = ["機密", "内部", "運用", "開発"];

export interface ParsedForumEntry {
  /** 解決した議論タイプ。タグから判定できなければ null (= 受理しない)。 */
  flow: FlowKind | null;
  /** 抽出した機密度+観点タグ。 */
  tags: FlowTag[];
}

/**
 * フォーラム適用タグ名から議論タイプ + フロータグを抽出する。
 * - 議論タイプ: タグ名を parseFlowKind で解決 (+ override の別名)。複数該当時は最初の解決を採用。
 * - フロータグ: 機密/内部/運用/開発 に一致する適用タグを抽出。
 */
export function parseForumEntry(appliedTagNames: string[], mapping?: ForumTagMapping): ParsedForumEntry {
  const flowTagNames = mapping?.flowTagNames ?? DEFAULT_FLOW_TAG_NAMES;

  // フロータグ抽出 (タグ名が FlowTag と一致)
  const tags: FlowTag[] = [];
  for (const name of appliedTagNames) {
    const hit = flowTagNames.find((t) => name.trim() === t || name.trim().includes(t));
    if (hit && !tags.includes(hit)) tags.push(hit as FlowTag);
  }

  // 議論タイプ判定: まず override の別名、次に標準 parseFlowKind
  let flow: FlowKind | null = null;
  const override = mapping?.flowKindTagNames;
  for (const name of appliedTagNames) {
    if (override) {
      const matched = (Object.entries(override) as [FlowKind, string[]][]).find(([, names]) =>
        names.some((n) => name.trim() === n || name.trim().includes(n))
      );
      if (matched) {
        flow = matched[0];
        break;
      }
    }
    const k = parseFlowKind(name);
    if (k) {
      flow = k;
      break;
    }
  }

  return { flow, tags };
}

/** フォーラム starter 投稿の最小入力 (gateway/forum-monitor 由来)。 */
export interface ForumPostInput {
  /** starter 投稿本文 = テーマ。 */
  theme: string;
  /** フォーラムの適用タグ名。 */
  appliedTagNames: string[];
  /** scene 文字列 (discord:<guild>/<thread>)。記録用。 */
  scene?: string;
}

export type ForumEntryResult =
  | { ok: true; dispatch: DispatchResult }
  | { ok: false; reason: "flow-required"; message: string };

/**
 * フォーラム投稿を受けてフローを起動する。
 * 議論タイプが適用タグから判定できなければ受理しない (ok:false, flow-required)。
 */
export async function handleForumFlowPost(
  input: ForumPostInput,
  deps: DispatchDeps,
  mapping?: ForumTagMapping
): Promise<ForumEntryResult> {
  const parsed = parseForumEntry(input.appliedTagNames, mapping);
  if (!parsed.flow) {
    return {
      ok: false,
      reason: "flow-required",
      message:
        "議論タイプのタグ (議論/改善/学習/壁打ち) が付いていません。フォーラムタグで議論タイプを指定してください。",
    };
  }
  const dispatch = await dispatchFlow(
    { theme: input.theme, tags: parsed.tags, flow: parsed.flow, scene: input.scene },
    deps
  );
  return { ok: true, dispatch };
}
