/**
 * Discord application command (slash) 定義 (single source of truth).
 *
 * 旧実装は handler (InteractionCreate) だけを持ち、コマンドを Discord に **登録**
 * していなかったため、クライアントの候補に slash が一切出てこなかった。本ファイルの
 * 定義を起動時 (gateway) と手動 script (scripts/register-discord-commands.ts) の
 * 双方から Discord REST `PUT /applications/{id}/commands` に流し込む。
 *
 * routeSlashCommand (command-router) が処理する name と必ず一致させること。
 */

import { ApplicationCommandOptionType } from "discord.js";
import type { RESTPostAPIChatInputApplicationCommandsJSONBody } from "discord.js";

export const DISCORD_COMMAND_DEFS: RESTPostAPIChatInputApplicationCommandsJSONBody[] = [
  {
    name: "propose",
    description: "hypothesis (跳躍的仮説) を提案して自走議論を起こす",
    options: [
      {
        name: "statement",
        description: "仮説を 1 文で",
        type: ApplicationCommandOptionType.String,
        required: true,
      },
    ],
  },
  {
    name: "validate",
    description: "hypothesis を検証する (theory=論理 / emotion=感情)",
    options: [
      {
        name: "mode",
        description: "theory または emotion",
        type: ApplicationCommandOptionType.String,
        required: true,
        choices: [
          { name: "theory", value: "theory" },
          { name: "emotion", value: "emotion" },
        ],
      },
    ],
  },
  { name: "integrate", description: "検証済 hypothesis を設計知見として統合 (採用)" },
  { name: "reject", description: "hypothesis を棄却" },
  {
    name: "discutere-status",
    description: "persona-engine の稼働状態を確認 (ephemeral)",
  },
  {
    name: "discutere-queue",
    description: "現在の議論キュー (進行中 session / 未処理 gap / 仮説) を可視化",
  },
  {
    name: "discutere-kill",
    description: "persona-engine の自走を停止/再開 (admin only)",
    options: [
      {
        name: "enabled",
        description: "true で再開、false で停止",
        type: ApplicationCommandOptionType.Boolean,
        required: true,
      },
    ],
  },
  {
    name: "discutere-backup",
    description: "学習データを今すぐ S3 にバックアップ (admin only)",
  },
  {
    name: "discutere-conclusions",
    description: "収束した議論の結論を一覧 / 指定で論述データ (議論ログ・止揚・高評価意見) を表示",
    options: [
      {
        name: "gap",
        description: "結論の gap id を指定すると裏の論述データを表示 (省略で一覧)",
        type: ApplicationCommandOptionType.String,
        required: false,
      },
    ],
  },
  {
    name: "debate",
    description: "議題でパーティ議論を開始 (司会+キーマン+意見、想定発話数で続行/停止を問う)",
    options: [
      {
        name: "topic",
        description: "議題 (例: ヴァンサバ風ゲームの面白さの核は?)",
        type: ApplicationCommandOptionType.String,
        required: true,
      },
    ],
  },
  {
    name: "persona-generate",
    description: "外部データ (クロール済の声) から憑依対象ペルソナを生成/更新する (admin only)",
    options: [
      {
        name: "source",
        description: "source 種別で絞る (例: youtube / steam。省略で全体)",
        type: ApplicationCommandOptionType.String,
        required: false,
      },
    ],
  },
  {
    name: "economy-graph",
    description: "ゲームの内部経済導線と遊びの関係性をグラフ分析する",
    options: [
      {
        name: "game",
        description: "ゲームタイトル (例: Hades, Hollow Knight, Balatro)",
        type: ApplicationCommandOptionType.String,
        required: true,
      },
    ],
  },
];
