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
];
