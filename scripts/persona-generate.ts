/**
 * ペルソナ生成 CLI (C — スタブ / プール投入 IF)。
 *
 * 実際の「学習データからの自動生成」ロジックは設計中 (考え中)。
 * 当面はプールへ手動投入して B(憑依)/F(合成)/G(壁打ち相手) を試せるようにする。
 *
 *   npm run persona:generate -- \
 *     --name "ローグ好き太郎" --style "ストイック" \
 *     --traits "高難度志向,上達快感" --affect-text "高難度を上達する達成感" \
 *     [--role opinion] [--source roguelike] [--label ...]
 *
 * affect ベクトルは --affect-text を sentiment-vector の 20 次元に変換して持たせる。
 * TODO: 学習データ (外部の声 / メカニクス) からの自動生成に置換する。
 */

import { randomUUID } from "node:crypto";
import { pathToFileURL } from "node:url";
import { insertPoolPersona } from "../src/flow/persona-pool.js";
import { textToVector } from "../src/flow/sentiment-vector.js";
import type { FlowRole } from "../src/flow/personas.js";

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

export function parseRole(raw: string | undefined): FlowRole {
  const role = raw ?? "opinion";
  if (role === "opinion" || role === "debater" || role === "facilitator") {
    return role;
  }
  throw new Error(`--role must be one of opinion|debater|facilitator, got: ${role}`);
}

async function main(): Promise<void> {
  const name = arg("name");
  const affectText = arg("affect-text");
  if (!name || !affectText) {
    console.error(
      '使い方: npm run persona:generate -- --name "名前" --affect-text "望む体験の説明" ' +
        '[--style 口調] [--traits a,b] [--role opinion|debater|facilitator] [--source 学習データ名] [--label ...]'
    );
    process.exit(1);
  }
  const role = parseRole(arg("role"));
  const traits = (arg("traits") ?? "").split(/[,、]/).map((s) => s.trim()).filter(Boolean);
  const persona = {
    id: randomUUID(),
    name,
    role,
    speechStyle: arg("style") ?? "",
    traits,
    affectVector: textToVector(affectText),
    origin: "generated" as const,
    parentIds: [],
    learningSource: arg("source"),
    label: arg("label"),
  };
  insertPoolPersona(persona);
  console.log(`✅ プール投入: ${persona.name} (id=${persona.id}, role=${persona.role}, source=${persona.learningSource ?? "-"})`);
  console.log("  ※ C の自動生成は設計中。当面は本コマンドで手動投入してプールを育てる。");
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((e) => {
    console.error(`NG: ${(e as Error).message}`);
    process.exit(1);
  });
}
