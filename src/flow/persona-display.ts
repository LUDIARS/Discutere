/**
 * 発話主体の表示名 composer (OVERVIEW §名前表示)。
 *
 * 露出面 (Discord webhook username / Slack username / WebUI の who) で
 * 「名前 (ロール名/憑依ペルソナ)」形式に統一する。憑依が無い場合は括弧内ロールのみ:
 *
 *   - 憑依なし: `陽介 (意見屋)`
 *   - 憑依あり: `陽介 (意見屋/論者#a1b2c3)`
 *
 * ロール名はそのターンの **スタンス** から決める (賛否はターン時に決まるため、現 `decideStance`
 * の結果を反映する)。スタンス未指定 (要約/結論など) なら role から決める。
 *
 * トランスポート非依存の純関数。Discord/Slack/WebUI すべてここを通す。
 */

import type { FlowRole, FlowStance } from "./personas.js";

/** スタンス → 表示ロール名。賛否はターン時に決まるのでスタンスを優先する。 */
export function stanceLabel(stance: FlowStance): string {
  switch (stance) {
    case "pro":
      return "賛成派";
    case "con":
      return "反対派";
    case "opinion":
      return "意見屋";
    case "neutral":
      return "進行役";
  }
}

/** role → 表示ロール名 (スタンスが無い文脈のフォールバック)。 */
export function roleLabel(role: FlowRole): string {
  switch (role) {
    case "facilitator":
      return "進行役";
    case "debater":
      return "論者";
    case "opinion":
      return "意見屋";
  }
}

export interface DisplayNameInput {
  /** 素のペルソナ名 (陽介 等)。 */
  name: string;
  /** そのターンのスタンス (pro/con/opinion/neutral)。あれば優先。 */
  stance?: FlowStance;
  /** ペルソナのロール (スタンス未指定時のフォールバック)。 */
  role?: FlowRole;
  /** 憑依しているデータ由来ペルソナの露出名 (論者#xxxxxx 等)。無ければ括弧内はロールのみ。 */
  possessionName?: string | null;
}

/**
 * 「名前 (ロール名/憑依ペルソナ)」を組み立てる。
 * 憑依が無ければ `名前 (ロール名)`。ロール名はスタンス優先、無ければ role、両方無ければ素の名前のみ。
 */
export function composeDisplayName(input: DisplayNameInput): string {
  const label = input.stance ? stanceLabel(input.stance) : input.role ? roleLabel(input.role) : "";
  const possession = input.possessionName?.trim();
  if (!label) return input.name;
  const inner = possession ? `${label}/${possession}` : label;
  return `${input.name} (${inner})`;
}
