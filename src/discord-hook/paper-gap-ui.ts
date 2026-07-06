import {
  ActionRowBuilder,
  ModalBuilder,
  TextInputBuilder,
  TextInputStyle,
} from "discord.js";

export const PAPER_GAP_COMMAND_NAME = "discutere-paper-gap";
export const PAPER_GAP_MODAL_PREFIX = "paper-gap";

export const PAPER_GAP_TYPES = [
  {
    value: "game_content",
    name: "ゲーム内容",
    description: "ゲーム内容・ルール・体験前提の不足",
  },
  {
    value: "user_reaction",
    name: "ユーザの反応",
    description: "レビュー・コメント・想定反応の不足",
  },
  {
    value: "spec",
    name: "仕様書",
    description: "仕様書や参考資料からの不足",
  },
  {
    value: "mechanics",
    name: "システム/メカニクス",
    description: "システムやメカニクス説明の不足",
  },
  {
    value: "project_code",
    name: "Anatomiaコード",
    description: "Anatomiaプロジェクトコードからの不足",
  },
  {
    value: "other",
    name: "その他",
    description: "分類しづらい不足事項",
  },
] as const;

export type PaperGapType = (typeof PAPER_GAP_TYPES)[number]["value"];

export const PAPER_GAP_TYPE_CHOICES = PAPER_GAP_TYPES.map((t) => ({
  name: t.name,
  value: t.value,
}));

export const PAPER_GAP_FIELD_IDS = {
  target: "target",
  detail: "detail",
  source: "source",
} as const;

const PAPER_GAP_TYPE_LABELS = new Map(PAPER_GAP_TYPES.map((t) => [t.value, t.name]));

export function isPaperGapType(value: string): value is PaperGapType {
  return PAPER_GAP_TYPE_LABELS.has(value as PaperGapType);
}

export function labelForPaperGapType(type: PaperGapType): string {
  return PAPER_GAP_TYPE_LABELS.get(type) ?? type;
}

export function buildPaperGapModal(input: {
  type: PaperGapType;
  channelId: string;
}): ModalBuilder {
  const label = labelForPaperGapType(input.type);
  const target = new TextInputBuilder()
    .setCustomId(PAPER_GAP_FIELD_IDS.target)
    .setLabel("対象・見出し")
    .setPlaceholder("例: バトルテンポ / 成長曲線 / 想定ユーザ")
    .setRequired(false)
    .setStyle(TextInputStyle.Short);
  const detail = new TextInputBuilder()
    .setCustomId(PAPER_GAP_FIELD_IDS.detail)
    .setLabel("追加・修正したい内容")
    .setPlaceholder("ペーパーに反映したい不足情報を具体的に入力してください")
    .setRequired(true)
    .setStyle(TextInputStyle.Paragraph);
  const source = new TextInputBuilder()
    .setCustomId(PAPER_GAP_FIELD_IDS.source)
    .setLabel("根拠・情報ソース")
    .setPlaceholder("URL、ファイル名、発言元など。なければ空欄")
    .setRequired(false)
    .setStyle(TextInputStyle.Short);

  return new ModalBuilder()
    .setCustomId(`${PAPER_GAP_MODAL_PREFIX}:${input.type}:${input.channelId}`)
    .setTitle(`不足情報: ${label}`)
    .addComponents(
      new ActionRowBuilder<TextInputBuilder>().addComponents(target),
      new ActionRowBuilder<TextInputBuilder>().addComponents(detail),
      new ActionRowBuilder<TextInputBuilder>().addComponents(source)
    );
}

export function parsePaperGapModalCustomId(
  customId: string
): { type: PaperGapType; channelId: string } | null {
  if (!customId.startsWith(`${PAPER_GAP_MODAL_PREFIX}:`)) return null;
  const rest = customId.slice(PAPER_GAP_MODAL_PREFIX.length + 1);
  const [rawType, channelId] = rest.split(":");
  if (!rawType || !channelId || !isPaperGapType(rawType)) return null;
  return { type: rawType, channelId };
}

export function formatPaperGapReviewInstruction(input: {
  type: PaperGapType;
  target?: string;
  detail: string;
  source?: string;
}): string {
  const lines = [
    `ディスカッションペーパーの不足事項として「${labelForPaperGapType(input.type)}」を追加・反映してください。`,
  ];
  if (input.target?.trim()) lines.push(`対象: ${input.target.trim()}`);
  lines.push(`内容: ${input.detail.trim()}`);
  if (input.source?.trim()) lines.push(`根拠・情報ソース: ${input.source.trim()}`);
  return lines.join("\n");
}
