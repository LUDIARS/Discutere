/**
 * ゲーム名の別名 (略称⇄正式名・和名⇄英名) 展開 (#301)。
 *
 * 外部の声 (クロール材料) は口語の略称で書かれることが多い (例「モンスト」4000 件超 / 正式名
 * 「モンスターストライク」は数十件)。議題語をそのまま検索すると正式名にしか当たらず材料を
 * 取りこぼす。ここで検索語を別名グループに展開してから LIKE 照合する。
 *
 * 別名の出所は 2 つ:
 *   1. games テーブルのタイトル "EN (JP)" / "JP (EN)" を機械分解 (英名・和名の相互補完)。
 *   2. 機械導出できない口語略称は静的辞書 (STATIC_ALIAS_GROUPS) で補う。
 * 純関数 — 単体テスト可能 (タイトル群は呼び出し側が KG から渡す)。
 */

/**
 * 機械導出できない口語略称グループ (同義語の配列)。lower-case 比較で突合する。
 * タイトル由来 (EN/JP) と共通要素があればマージされるので、ここには「略称」を中心に列挙する。
 */
export const STATIC_ALIAS_GROUPS: readonly (readonly string[])[] = [
  ["モンスターストライク", "モンスト"],
  ["death stranding", "デス・ストランディング", "デスストランディング", "デススト"],
  ["vampire survivors", "ヴァンパイアサバイバーズ", "ヴァンサバ"],
  ["hollow knight", "ホロウナイト", "ホロナイ"],
  ["genshin impact", "原神", "genshin"],
  ["shadow of the colossus", "ワンダと巨像", "ワンダ"],
];

/** "EN (JP)" / "JP (英名)" タイトルを別名 (外側 + 括弧内) に分解する。 */
export function parseTitleAliases(title: string): string[] {
  const out: string[] = [];
  const outer = title.split(/[（(]/)[0].trim();
  if (outer.length >= 2) out.push(outer);
  const inner = title.match(/[（(]([^）)]+)[）)]/);
  if (inner && inner[1].trim().length >= 2) out.push(inner[1].trim());
  return out;
}

/** 2 グループが (lower-case で) 共通要素を持つか。 */
function shares(a: Set<string>, b: readonly string[]): boolean {
  return b.some((x) => a.has(x.toLowerCase()));
}

/**
 * games タイトル群 + 静的略称をマージした別名グループ集合を作る。
 * 共通要素を持つグループは 1 つに畳む (例: タイトル「Genshin Impact (原神)」と静的 [genshin...] が原神で繋がる)。
 * @param extraGroups 追加の別名グループ (例: Ludus game-lexicon 由来の用語辞書
 *   `LUDUS_TERM_ALIAS_GROUPS`)。ゲーム名だけでなく「経験値⇄XP⇄EXP」のような
 *   用語の言い換えも検索拡張に乗せる。
 */
export function buildAliasGroups(
  titles: readonly string[],
  extraGroups: readonly (readonly string[])[] = []
): string[][] {
  const groups: string[][] = [];
  const lowerSets: Set<string>[] = [];

  const addGroup = (members: readonly string[]): void => {
    const cleaned = members.map((m) => m.trim()).filter((m) => m.length >= 2);
    if (cleaned.length === 0) return;
    const lower = cleaned.map((m) => m.toLowerCase());
    // 既存グループと共通要素があればマージ。
    const idx = lowerSets.findIndex((s) => shares(s, cleaned));
    if (idx >= 0) {
      for (let i = 0; i < cleaned.length; i++) {
        if (!lowerSets[idx].has(lower[i])) {
          groups[idx].push(cleaned[i]);
          lowerSets[idx].add(lower[i]);
        }
      }
    } else {
      groups.push([...cleaned]);
      lowerSets.push(new Set(lower));
    }
  };

  for (const title of titles) addGroup(parseTitleAliases(title));
  for (const g of STATIC_ALIAS_GROUPS) addGroup(g);
  for (const g of extraGroups) addGroup(g);
  // 単一要素グループ (別名が無い) は展開に寄与しないので落とす。
  return groups.filter((g) => g.length >= 2);
}

/** 語 t が別名グループ member と関連するか (完全一致 or 部分一致、短語の誤接続を避ける)。 */
function relates(t: string, member: string): boolean {
  if (t === member) return true;
  // 2〜3 文字の短い語での部分一致は誤爆しやすいので、 4 文字以上のときだけ部分一致を許す。
  if (member.length >= 4 && t.includes(member)) return true;
  if (t.length >= 4 && member.includes(t)) return true;
  return false;
}

/**
 * 検索語を別名で拡張する。元の語は保持し、関連グループの同義語を加える。
 * @param terms 検索語 (extractKeyTerms / topicTermsFromTitle の出力)。
 * @param groups buildAliasGroups の結果。
 * @param max 返す語の最大数 (LIKE 句の暴発を防ぐ)。
 */
export function expandAliases(
  terms: readonly string[],
  groups: readonly (readonly string[])[],
  max = 12
): string[] {
  const out = new Set<string>();
  for (const raw of terms) {
    const t = raw.trim().toLowerCase();
    if (!t) continue;
    out.add(t);
    for (const g of groups) {
      if (g.some((m) => relates(t, m.toLowerCase()))) {
        for (const m of g) out.add(m.toLowerCase());
      }
    }
  }
  return [...out].slice(0, max);
}
