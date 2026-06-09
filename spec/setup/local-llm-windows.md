# セットアップ: Windows で Di をローカル LLM (Gemma 4 12B) で動かす

Discutere (Di) を **Claude ではなくローカル LLM** で動かすための Windows 用手順。
想定モデルは **Gemma 4 12B** (テキスト専用 / 256K context、 Apache 2.0 ベース)。
LLM backend は `backend=local`(OpenAI 互換 `/v1/chat/completions`)で、 Ollama を
サーバに使う。 実装の正本は `src/persona-engine/llm/local-openai.ts` / `src/config.ts`。

> FT (ファインチューニング) は Claude の API/CLI に学習エンドポイントが無いため不可。
> ローカルでモデルを使うのはこの `local` backend 経由。 既存 Claude 経路は無改変。

---

## 0. 必要環境

| 項目 | 要件 | このマシンの実測 (2026-06-09) |
|---|---|---|
| OS | Windows 10/11 | Windows 11 ✅ |
| Node.js | 20+ (Di は tsx で起動) | v24 ✅ |
| Ollama | 最新 (Windows 版) | **導入済 0.30.6** (winget、 2026-06-09) ✅ |
| メモリ/VRAM | 12B Q4 量子化で **約 8〜10GB**。 GPU 無しでも RAM 16GB+ で CPU 動作可 (低速) | — |
| ディスク | モデル DL に **約 8GB** | — |

---

## 1. Ollama を入れる

いずれか:

```powershell
winget install --id Ollama.Ollama -e --accept-source-agreements --accept-package-agreements
```

winget が使えない / 失敗する場合は公式インストーラ `https://ollama.com/download/windows` を実行。
インストール後、 Ollama は **バックグラウンドサービスとして常駐**し `http://localhost:11434`
で待ち受ける(タスクトレイに常駐アイコン)。 確認:

```powershell
ollama --version
# サーバ疎通 (空でも 200 が返れば OK):
curl http://localhost:11434/api/tags
```

> サービスが起動していない時は `ollama serve` を手動で 1 回起動(別ウィンドウで常駐)。

---

## 2. Gemma 4 12B を取得

```powershell
ollama pull gemma4:12b
```

- **タグは必ず実機で確認**: Gemma 4 は新しく、 Ollama のタグ名が `gemma4:12b` でない / 未公開の
  可能性がある。 `ollama pull` がエラーになったら以下で確認・代替する:
  - `ollama list` … 取得済みモデル一覧
  - Ollama ライブラリ (`https://ollama.com/library`) で `gemma` の現行タグを確認
  - 公式が GGUF を配布していれば、 HuggingFace から落として `ollama create gemma4-12b -f Modelfile`
    で取り込む(`Modelfile` に `FROM ./gemma-4-12b-it-Q4_K_M.gguf`)。
  - 暫定で前世代 `gemma3:12b` でも `local` backend は動く(モデル差は config だけ)。
- 取得後の動作確認(任意):
  ```powershell
  ollama run gemma4:12b "一言で自己紹介して"
  ```

---

## 3. Di を local backend に設定

`discutere.config.json`(無ければ `discutere.config.example.json` を複製)で:

```jsonc
{
  "llm": {
    "backend": "local",
    "local": {
      "baseUrl": "http://localhost:11434/v1",
      "model": "gemma4:12b",
      "apiKey": "",
      "timeoutMs": 120000
    }
  },
  "classifier": { "backend": "local" }   // 分類器もローカルにする場合 (model は classifier.model で上書き可)
}
```

env でも指定可(config より優先):

```powershell
$env:LLM_BACKEND = "local"
$env:LLM_LOCAL_BASE_URL = "http://localhost:11434/v1"
$env:LLM_LOCAL_MODEL = "gemma4:12b"
```

> 設定キーの正本は `src/config.ts` の `llm.local` / `LlmBackend = ... | "local"`。
> persona-engine / classifier / summarizer の全 dispatch が `backend=local` で切り替わる。

---

## 4. 疎通スモークテスト

```powershell
npm run llm:smoke
# プロンプト差し替え: npm run llm:smoke -- "テスト発話"
```

- `OK (xxxms)` + 応答本文が出れば Di からローカル Gemma に繋がっている。
- `NG`: baseUrl/model を確認(Step 2 でモデルが取得済みか、 タグが正しいか)。

---

## 5. 起動

```powershell
npm run dev
```

起動ログに `persona-engine LLM: LocalOpenAiClient (http://localhost:11434/v1 / model=gemma4:12b)`
が出れば OK。

---

## トラブルシュート

- **`ollama pull` でタグが見つからない**: §2 の代替(`ollama list` / library 確認 / GGUF 取り込み /
  暫定 gemma3)。
- **応答が遅い / タイムアウト**: GPU 無し CPU 動作だと 12B は遅い。 `llm.local.timeoutMs` を
  伸ばす、 量子化を下げる(Q4 → Q4_K_M / Q3)、 または小さいモデル(例 4B)で動作確認してから 12B へ。
- **VRAM 不足**: 量子化を下げる / `OLLAMA_NUM_GPU` で GPU レイヤ数を調整 / CPU フォールバック。
- **port 11434 に繋がらない**: Ollama サービス起動確認 (`ollama serve`)。 Windows の TCP dynamic
  port range が広すぎると loopback が壊れることがある(`netsh int ipv4 show dynamicport tcp`、
  既定 49152-16384 へ)。
- **Di が anthropic のまま**: `config.llm.backend` が `local` になっているか / env `LLM_BACKEND` の
  優先順位を確認(env > config file > default)。
- **応答が空になる (reasoning モデル)**: **Gemma 4 は思考モデル**。 Ollama の OpenAI 互換応答で
  `message.reasoning`(思考)と `message.content`(最終回答)が分かれ、 **`max_tokens` が小さいと
  思考でトークンを使い切り content が空**になる(実機: 128 だと空・512 で回答)。 Di の
  `LocalOpenAiClient` は既定 `max_tokens=4096` に引き上げ済(空応答防止)。 さらに長い議題で空に
  なるなら呼び出し側で `maxTokens` を増やす。 `ollama run gemma4:12b "..."` で素の動作確認も可。

---

## 実行ログ (このマシンで実機確認 / 2026-06-09)

「できる範囲で実行」 した結果:

- **Ollama 導入**: `winget install --id Ollama.Ollama` で成功 → `ollama version 0.30.6`
  (`%LOCALAPPDATA%\Programs\Ollama\ollama.exe`)。
- **サーバ**: `http://localhost:11434` で UP (api/version=0.30.6) を確認。
- **タグ確認**: `ollama pull gemma4:12b` の manifest が解決し DL 開始 →
  **`gemma4:12b` は Ollama の有効タグ**(全 7.4GB)。 = Di の既定 `llm.local.model="gemma4:12b"`
  はそのまま使える。
- **残**: 回線速度の都合で DL に時間がかかるため、 DL 完了後に `npm run llm:smoke` で
  Di ↔ ローカル Gemma の疎通を確認する(本ガイド §4)。設定は `backend=local` を指定するだけ。

## 参照
- 実装: `src/persona-engine/llm/local-openai.ts`(LocalOpenAiClient)/ `src/config.ts`(llm.local)
- 疎通: `scripts/llm-smoke.ts`(`npm run llm:smoke`)
- backend 設計: `CLAUDE.md` § ローカル LLM backend
- モデル: Gemma 4 コア `https://ai.google.dev/gemma/docs/core`
