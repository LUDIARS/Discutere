/**
 * フォーラム議論タグ (新フロー入口 — T7 経路 B)。
 *
 * Discord フォーラムを「議論の UI」として使うため、Di が必要とする定義済みタグを
 * フォーラムの applied tags として用意する:
 *   - **議論タイプ (必須・1 つ選択)**: 議論 / 改善 / 学習 / 壁打ち
 *     → `parseFlowKind` が解決できる正準名。dispatch の FlowKind に対応。
 *   - **機密度+観点 (任意・複数可)**: 機密 / 内部 / 運用 / 開発
 *     → `FlowTag` と一致。外部収集可否・ペーパー補足を決める。
 *
 * 起動時 (ClientReady) に議論フォーラムを ensure し、不足タグを補充する。
 * 議論タイプタグが付いていない投稿には、タグ選択 UI (StringSelectMenu) を出す。
 */

import {
  ChannelType,
  ActionRowBuilder,
  ButtonBuilder,
  ButtonStyle,
  ModalBuilder,
  StringSelectMenuBuilder,
  StringSelectMenuOptionBuilder,
  TextInputBuilder,
  TextInputStyle,
  type Client,
  type ForumChannel,
  type GuildForumTagData,
} from "discord.js";

/** 議論タイプ (必須・1 つ選択) のフォーラムタグ名。順序は select UI の表示順。 */
export const FLOW_KIND_TAG_NAMES = ["議論", "改善", "学習", "壁打ち"] as const;

/** 機密度+観点 (任意・複数可) のフォーラムタグ名。FlowTag と一致。 */
export const FLOW_ASPECT_TAG_NAMES = ["機密", "内部", "運用", "開発"] as const;

/** Di が議論フォーラムに用意する全タグ名。 */
export const ALL_FLOW_TAG_NAMES: readonly string[] = [
  ...FLOW_KIND_TAG_NAMES,
  ...FLOW_ASPECT_TAG_NAMES,
];

/** 議論タイプ select の customId 接頭辞 (`flow-pick:<threadId>`)。 */
export const FLOW_PICK_PREFIX = "flow-pick";

/** select 値 (= FlowKind ラベル) と表示文。 */
const FLOW_KIND_CHOICES: Array<{ value: string; label: string; description: string }> = [
  { value: "議論", label: "議論", description: "中立投票で世論をまとめる" },
  { value: "改善", label: "改善", description: "課題抽出 + 改善案 (design_gap スコア)" },
  { value: "学習", label: "学習", description: "外部の声を収集して KG に取り込む" },
  { value: "壁打ち", label: "壁打ち", description: "あなたの発言に AI が応答し続ける" },
];

/**
 * 議論タイプ select メニュー (1 つ選択)。タグ無し投稿に出す。
 * customId は `flow-pick:<threadId>`。選択値は FlowKind ラベル。
 */
export function buildFlowPickMenu(threadId: string): ActionRowBuilder<StringSelectMenuBuilder> {
  const menu = new StringSelectMenuBuilder()
    .setCustomId(`${FLOW_PICK_PREFIX}:${threadId}`)
    .setPlaceholder("議論タイプを選択 (必須)")
    .setMinValues(1)
    .setMaxValues(1)
    .addOptions(
      FLOW_KIND_CHOICES.map((c) =>
        new StringSelectMenuOptionBuilder().setValue(c.value).setLabel(c.label).setDescription(c.description)
      )
    );
  return new ActionRowBuilder<StringSelectMenuBuilder>().addComponents(menu);
}

/** `flow-pick:<threadId>` の customId から threadId を取り出す。一致しなければ null。 */
export function parseFlowPickCustomId(customId: string): string | null {
  if (!customId.startsWith(`${FLOW_PICK_PREFIX}:`)) return null;
  const threadId = customId.slice(FLOW_PICK_PREFIX.length + 1);
  return threadId.length > 0 ? threadId : null;
}

// ── 進行量設定 UI (item1: ラウンド/ターン数を議論ごとに毎回変える) ───────────────

/** 進行量プリセット select の customId 接頭辞 (`flow-settings:<threadId>`)。 */
export const FLOW_SETTINGS_PREFIX = "flow-settings";
/** 「数値を指定」ボタンの customId 接頭辞 (`flow-custom:<threadId>`)。 */
export const FLOW_CUSTOM_BTN_PREFIX = "flow-custom";
/** 進行量モーダルの customId 接頭辞 (`flow-modal:<threadId>`)。 */
export const FLOW_MODAL_PREFIX = "flow-modal";

/** 進行量プリセット (rounds×turns)。"default" は config 既定を使う。 */
const FLOW_SETTINGS_PRESETS: Array<{ value: string; label: string; description: string }> = [
  { value: "default", label: "標準で開始", description: "設定ファイルの既定ラウンド/ターン数で開始" },
  { value: "2x4", label: "さくっと (2R×4T)", description: "短時間・低コスト" },
  { value: "3x6", label: "標準量 (3R×6T)", description: "バランス型" },
  { value: "5x8", label: "じっくり (5R×8T)", description: "論点を深掘り" },
  { value: "8x12", label: "とことん (8R×12T)", description: "大ボリューム (高コスト)" },
];

/**
 * 進行量設定メッセージの components (プリセット select + 「数値を指定」ボタン)。
 * discussion/improvement を起動する前に出し、選択/入力でフローを開始する (item1)。
 */
export function buildFlowSettingsComponents(
  threadId: string
): [ActionRowBuilder<StringSelectMenuBuilder>, ActionRowBuilder<ButtonBuilder>] {
  const menu = new StringSelectMenuBuilder()
    .setCustomId(`${FLOW_SETTINGS_PREFIX}:${threadId}`)
    .setPlaceholder("進行量を選んで開始 (ラウンド数 × ターン数)")
    .setMinValues(1)
    .setMaxValues(1)
    .addOptions(
      FLOW_SETTINGS_PRESETS.map((p) =>
        new StringSelectMenuOptionBuilder().setValue(p.value).setLabel(p.label).setDescription(p.description)
      )
    );
  const button = new ButtonBuilder()
    .setCustomId(`${FLOW_CUSTOM_BTN_PREFIX}:${threadId}`)
    .setLabel("数値を指定して開始")
    .setEmoji("⚙️")
    .setStyle(ButtonStyle.Secondary);
  return [
    new ActionRowBuilder<StringSelectMenuBuilder>().addComponents(menu),
    new ActionRowBuilder<ButtonBuilder>().addComponents(button),
  ];
}

/** ラウンド/ターン数を入力するモーダル (item1: 任意の数値指定)。 */
export function buildFlowSettingsModal(threadId: string): ModalBuilder {
  const rounds = new TextInputBuilder()
    .setCustomId("rounds")
    .setLabel("ラウンド数 (1〜10)")
    .setPlaceholder("例: 3")
    .setRequired(false)
    .setStyle(TextInputStyle.Short);
  const turns = new TextInputBuilder()
    .setCustomId("turns")
    .setLabel("1ラウンドのターン数 (1〜20)")
    .setPlaceholder("例: 6")
    .setRequired(false)
    .setStyle(TextInputStyle.Short);
  return new ModalBuilder()
    .setCustomId(`${FLOW_MODAL_PREFIX}:${threadId}`)
    .setTitle("進行量を指定")
    .addComponents(
      new ActionRowBuilder<TextInputBuilder>().addComponents(rounds),
      new ActionRowBuilder<TextInputBuilder>().addComponents(turns)
    );
}

/** `<prefix>:<threadId>` の customId から threadId を取り出す。 */
function parsePrefixedCustomId(prefix: string, customId: string): string | null {
  if (!customId.startsWith(`${prefix}:`)) return null;
  const id = customId.slice(prefix.length + 1);
  return id.length > 0 ? id : null;
}

export const parseFlowSettingsCustomId = (id: string): string | null =>
  parsePrefixedCustomId(FLOW_SETTINGS_PREFIX, id);
export const parseFlowCustomBtnCustomId = (id: string): string | null =>
  parsePrefixedCustomId(FLOW_CUSTOM_BTN_PREFIX, id);
export const parseFlowModalCustomId = (id: string): string | null =>
  parsePrefixedCustomId(FLOW_MODAL_PREFIX, id);

/**
 * プリセット値 ("3x6" / "default") を rounds/turns に解決する。
 * "default" / 不正は {} (= config 既定)。
 */
export function parseSettingsPreset(value: string): { rounds?: number; turnsPerRound?: number } {
  const m = value.match(/^(\d+)x(\d+)$/);
  if (!m) return {};
  return { rounds: Number(m[1]), turnsPerRound: Number(m[2]) };
}

/**
 * 既存タグに不足分を足したタグ配列を返す (純粋関数)。
 * Discord のフォーラムタグ上限は 20。既存タグは温存し、ALL_FLOW_TAG_NAMES の
 * 未登録分だけを末尾に追加する。上限を超える分は追加しない。
 */
export function mergeForumTags(
  existing: ReadonlyArray<{ name: string }>,
  want: readonly string[] = ALL_FLOW_TAG_NAMES
): { tags: GuildForumTagData[]; added: string[] } {
  const MAX = 20;
  const have = new Set(existing.map((t) => t.name));
  const tags: GuildForumTagData[] = existing.map((t) => ({ name: t.name }));
  const added: string[] = [];
  for (const name of want) {
    if (have.has(name)) continue;
    if (tags.length >= MAX) break;
    tags.push({ name });
    have.add(name);
    added.push(name);
  }
  return { tags, added };
}

/**
 * フォーラムの availableTags に Di の定義済みタグを補充する。
 * 追加が無ければ何もしない (= API を叩かない)。Manage Channels 権限が要る。
 */
export async function ensureForumFlowTags(forum: ForumChannel): Promise<string[]> {
  const { tags, added } = mergeForumTags(forum.availableTags ?? []);
  if (added.length === 0) return [];
  await forum.setAvailableTags(tags);
  return added;
}

/**
 * guild に議論フォーラムを ensure する。
 * - 同名の GuildForum が在れば流用し、不足タグを補充する。
 * - 無ければ全タグ付きで新規作成する (Manage Channels 権限が要る)。
 * 権限不足・作成失敗は null を返す (caller が skip)。
 */
export async function ensureDiscussionForum(
  client: Client,
  guildId: string,
  forumName: string
): Promise<ForumChannel | null> {
  let guild;
  try {
    guild = await client.guilds.fetch(guildId);
  } catch {
    return null;
  }

  // 既存の同名フォーラムを探す。
  try {
    const channels = await guild.channels.fetch();
    const existing = channels.find(
      (c): c is ForumChannel => !!c && c.type === ChannelType.GuildForum && c.name === forumName
    );
    if (existing) {
      const added = await ensureForumFlowTags(existing).catch(() => []);
      if (added.length > 0) {
        console.log(`  forum-tags: #${forumName} にタグ補充 [${added.join(", ")}]`);
      }
      return existing;
    }
  } catch {
    /* fetch 失敗 → 作成を試みる */
  }

  // 無ければ全タグ付きで作成する。
  try {
    const { tags } = mergeForumTags([]);
    const created = (await guild.channels.create({
      name: forumName,
      type: ChannelType.GuildForum,
      availableTags: tags,
      topic: "議論したいテーマを投稿してください。議論タイプ (議論/改善/学習/壁打ち) のタグを 1 つ付けます。",
      reason: "Discutere 議論フォーラム (新フロー入口)",
    })) as ForumChannel;
    console.log(`  forum-tags: 議論フォーラム #${forumName} を作成 (タグ ${tags.length} 件)`);
    return created;
  } catch (err) {
    console.warn(`  forum-tags: 議論フォーラム作成失敗 (${forumName}): ${(err as Error).message}`);
    return null;
  }
}
