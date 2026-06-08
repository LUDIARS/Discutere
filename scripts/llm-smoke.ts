/**
 * ローカル LLM (OpenAI 互換) 疎通スモークテスト。
 *
 *   npm run llm:smoke            # config.llm.local の設定で 1 回叩いて結果を表示
 *   npm run llm:smoke -- "プロンプト"   # プロンプト差し替え
 *
 * Gemma 等をローカル (Ollama/vLLM/LM Studio) で起動した後、 Discutere から
 * 実際に応答が返るかをワンコマンドで確認するための疎通チェック。
 * baseUrl/model/apiKey は config (llm.local) / env (LLM_LOCAL_*) を読む。
 */
import { getConfig } from "../src/config.js";
import { LocalOpenAiClient } from "../src/persona-engine/llm/local-openai.js";

async function main(): Promise<void> {
  const { local } = getConfig().llm;
  const prompt = process.argv.slice(2).join(" ").trim() || "こんにちは。一言で自己紹介して。";

  console.log("== local LLM smoke ==");
  console.log(`baseUrl : ${local.baseUrl}`);
  console.log(`model   : ${local.model}`);
  console.log(`apiKey  : ${local.apiKey ? "(set)" : "(none)"}`);
  console.log(`prompt  : ${prompt}`);
  console.log("---");

  const client = new LocalOpenAiClient({
    baseUrl: local.baseUrl,
    defaultModel: local.model,
    apiKey: local.apiKey,
    defaultTimeoutMs: local.timeoutMs,
  });

  const started = Date.now();
  const res = await client.invoke({ prompt, maxTokens: 256 });
  const ms = Date.now() - started;

  if (!res.ok) {
    console.error(`NG (${ms}ms): ${res.error}`);
    console.error(
      "→ エンドポイントが起動しているか / baseUrl・model が正しいか確認 " +
        "(Ollama: `ollama serve` + `ollama pull <model>` / vLLM 等は baseUrl を差し替え)。"
    );
    process.exitCode = 1;
    return;
  }

  console.log(`OK (${ms}ms)`);
  console.log(`usage   : in=${res.usage?.input_tokens ?? "?"} out=${res.usage?.output_tokens ?? "?"}`);
  console.log("response:");
  console.log(res.text);
}

void main();
