# Voluptas × Discutere 統合 — 仕様・実装計画書

> 2026-07-12 | Version 1.0
> 先行文書: [`docs/voluptas-integration-review.md`](./voluptas-integration-review.md) (設計・実現可能性レビュー)
> **読者 = 実装担当の AI エージェント (GPT-5.6) および人間レビュアー。**
> 本書は実装担当がこの 1 本だけで着手できるよう、背景・確定判断・タスク仕様・受け入れ条件・実装上の配慮を自己完結で記載する。

---

## 0. 実装担当 (GPT-5.6) への前提説明 — 最初に読むこと

### 0.1 関与リポジトリ

| リポジトリ | 役割 | 言語/形式 | 本計画での変更 |
|-----------|------|-----------|----------------|
| `LUDIARS/Voluptas` | プレイヤープロファイルサーバ (player-profile-server) | **CommonJS** / Express 4 / PostgreSQL / `node --test` | **主戦場**。認証再設計、感情タイムライン、嗜好統合 |
| `LUDIARS/Discutere` | Discord 議論 ChatBot。感情分析・ペルソナ資産の供給元 | **ESM / TypeScript** / tsx / kuzu+SQLite | sentiment-core の切り出し、コレクタの時刻保持 (**最小限の変更**) |
| `LUDIARS/Cernere` | 汎用認証プラットフォーム | TypeScript / uWebSockets.js | **変更しない** (SDK `@ludiars/cernere-service-adapter` を消費するのみ) |
| `LUDIARS/Lapilli` | LUDIARS 共有パッケージ monorepo (`packages/llm-gateway` 等) | TypeScript | 新パッケージ `packages/sentiment-core` の受け皿 (WS-B) |

### 0.2 絶対に守る制約 (MUST / MUST NOT)

これらはレビューで確定した規範的判断。実装中に「もっと良い案」を思いついても**逸脱せず、提案として PR 説明に書くに留める**こと。

1. **MUST NOT: 感情空間を新設・改変しない。** 感情ベクトルは Discutere の固定 20 次元 (`VECTOR_SPEC`: valence/arousal + Plutchik 8 + アスペクト 8 + meta 2、各 0..1) が唯一の正本。次元の追加・削除・並び替え・意味変更は禁止。不足を感じたら実装せず課題として報告する。
2. **MUST NOT: 嗜好次元を統一しない。** Voluptas の 12 次元 `preference_vector` と Discutere の 15 軸 `PREFERENCE_AXES` は**別レイヤとして併存**させる。相互の写像・変換テーブルを実装しない。
3. **MUST NOT: Discutere に HTTP 認証層を追加しない。** Discutere の認証境界は Discord Gateway + admin allowlist が canonical (`CLAUDE.md` 参照)。Voluptas の JWT を Discutere に持ち込まない。
4. **MUST: Discutere の匿名 workspace 方針を守る。** Voluptas のユーザ識別子 (SID・email・IdP sub) を Discutere 側ストア (KG / SQLite) に**生のまま**書かない。持ち込みは疑似 ID `ext:voluptas:<hash>` のみ (→ C-4)。
5. **MUST: SID (Voluptas 発行 UUID) を全データの主キーに保つ。** 認証プロバイダの追加・削除でユーザデータが SID から剥がれない構造を維持する (切り離し運用の担保)。
6. **MUST: graceful degrade。** LLM・外部 API・Redis の失敗で主機能 (ログイン、ログ取り込み、議論) を止めない。フォールバックを常に定義する (Discutere の既存原則と同じ)。
7. **MUST: 派生値と生ログの分離。** 感情タイムライン等の派生データは生イベント/生コメントから再計算可能にする (アルゴリズム改版で作り直せる)。
8. **MUST NOT: 実装しない領域 (§6 非目標) に手を出さない。** 表情ベース感情推定、任意ゲーム映像の VLM 自動展開認識、ASR は本計画のスコープ外。

### 0.3 リポジトリ別のコーディング規約・検証方法

- **Voluptas**: CommonJS (`require`)。既存のレスポンスエンベロープ `{ ok, data } / { ok: false, error: { code, message } }` を厳守。テストは `node --test` (`npm test`、`src/**/*.test.js`)。マイグレーションは `migrations/NNN_name.sql` 連番 + `npm run migrate`。新規依存は最小限に (現状 express/pg/jsonwebtoken/jwks-rsa/uuid/helmet/cors/express-rate-limit/node-jose のみ)。
- **Discutere**: ESM/TS。`npm run typecheck` 必須。テストは領域別ランナー (`npm run test:crawler` 等、vitest ではなく自作 run.ts)。**既存挙動のバイト等価を壊す変更は golden テストで担保** (WS-B)。設定は `src/config.ts` の typed config (default < `discutere.config.json` < env) に追加する。env 直読みを散らかさない。
- **共有パッケージ (sentiment-core)**: TypeScript で書き、**CJS + ESM の dual ビルド** (Voluptas が CJS のため)。ランタイム依存ゼロを目標 (lexicon/vocabulary は JSON 同梱)。
- **秘密情報**: token/secret はログに出さない。`.env.example` にキー名のみ追記。実値をコミットしない。

### 0.4 判断に迷ったときのポリシー

- 本書に仕様がある → そのまま実装。
- 本書に無いが自明 (エラーコード名、変数名等) → 既存コードの慣習に合わせて自分で決める。
- 本書に無く設計に影響する (スキーマ変更、外部 API 追加、制約 0.2 との緊張) → **実装せず**、PR 説明または issue に「未決事項」として選択肢つきで書く。勝手に決めない。

---

## 1. 背景 (要約)

Voluptas はゲームプレイヤーの嗜好・プレイログを管理するサーバで、(1) 認証基盤 Cernere の導入 (ただし切り離し運用のため Google 等の直連合も維持)、(2) Discutere の感情分析・ペルソナ実装の転用、(3) プレイ動画ベースのタイムライン感情曲線と「面白さ」の定量化、を行う。レビュー (`voluptas-integration-review.md`) の結論:

- (1) は Identity Source 抽象境界で実現可能。ただし現行 auth 実装に修正必須の穴が複数ある (WS-A)。
- (2) は Discutere から 3 部品 (20 次元ベクトル化 / 話者プロファイル / アンケートスコアリング) を切り出して転用 (WS-B, WS-C)。
- (3) は「タイムスタンプ付き視聴者コメント × ビート単位の感情スクリプト × 時系列 DesignGap」構成で実現する (WS-D)。動画映像/表情の直接解析はしない。

---

## 2. ワークストリームと依存関係

```
WS-A 認証 (Voluptas)          WS-B sentiment-core 切出し (Discutere→Lapilli)
  A1 既存auth修正                B1 パッケージ新設
  A2 IdentitySource抽象化        B2 Discutere側を差し替え (golden test)
  A3 CernereSource               │
      │                          ▼
      │                 WS-C 嗜好・ペルソナ (Voluptas)     WS-D 感情タイムライン
      │                   C1 sentiment-core 組込み          D1 コレクタ時刻保持 (Discutere)
      └──────────────▶   C2 アンケート15軸スコア            D2 affect_timelines (Voluptas)
       (認証済みAPIとして)  C3 統合プロファイルAPI            D3 game_beat_scripts + 目標曲線
                           C4 疑似ID境界                    D4 時系列 DesignGap API
                           C5 analysisEngine 誠実化          D5 計装ゲームのログ同期 (P3)
```

**実装順序 (推奨マイルストーン)**:

| Phase | 内容 | 完了条件 |
|-------|------|----------|
| **M1** | A1 (auth 修正) + B1/B2 (sentiment-core) | Voluptas auth の穴が塞がり、Discutere が共有パッケージ経由で従来と同一出力 |
| **M2** | A2/A3 (Cernere) + C1〜C5 | Cernere 有効/無効の両モードでログイン可能。統合プロファイル API が 3 空間併記で返る |
| **M3** | D1〜D4 | 実在の niconico/YouTube 動画からゲームの観測感情曲線と Gap が API で取れる |
| **M4** | D5 | 計装ゲーム (Ludellus) セッションのログ同期タイムライン |

各 WS は独立性が高いので、**1 PR = 1 タスク (A1-1 単位)** を基本とし、巨大 PR を作らないこと。

---

## 3. WS-A: 認証再設計 (Voluptas)

### A1. 既存 auth の修正 (Cernere 以前の必須修正)

#### A1-1. OAuth state ストアの Redis 化

- 現状: `src/routes/auth.js:15` の in-memory `Map`。複数インスタンスで破綻。
- 仕様: Redis キー `oauth_state:<state>` に JSON (`provider`, `createdAt`)、TTL 600 秒。`SET NX` で保存、callback で `GETDEL` (one-shot 消費)。Redis 未設定時 (`REDIS_URL` 無し) は現行 Map にフォールバックし、起動ログに警告を 1 行出す (dev 利便性のため)。
- 依存追加: `ioredis` (Cernere と同系)。接続は lazy、失敗時はフォールバック (0.2-6)。
- 受け入れ: state の再利用が 400 になるテスト。TTL 超過 state の拒否テスト。

#### A1-2. 「OIDC OP」の看板を下ろす (設計書と実装の整合)

- 決定: Voluptas は OpenID Provider を自作**しない**。「IdP フェデレーション + 自サービス発行 JWT (RS256+JWKS)」と再定義する。
- 作業: `/auth/login` で保存している `code_challenge`/`code_challenge_method` (検証されておらず死にデータ) を削除。`/auth/.well-known/openid-configuration` は自サービスが OP でない以上誤解を招くため**削除** (JWKS 配信 `/auth/.well-known/jwks.json` は resource server 検証用に残す)。`player-profile-backend-design.md` §3 の記述を「OIDC 型」から「IdP フェデレーション型」に改訂し、改訂履歴に追記。
- 受け入れ: 既存の `/auth/token` (PKCE を IdP へ横流しするネイティブアプリ向けフロー) は挙動不変。

#### A1-3. ブラウザ向けログインフローの成立 (login ticket 方式)

- 現状: `/auth/callback` が JSON でトークンを返すため、redirect で着地する SPA がトークンを受け取れない。
- 仕様:
  1. `/auth/callback` は SID 解決まで従来通り行った後、**トークンを返さず** one-time ticket (`crypto.randomBytes(32)`) を発行して Redis `login_ticket:<ticket>` (値=userId, TTL 60 秒) に保存し、`302 → ${FRONTEND_URL}/auth/complete#ticket=<ticket>` へリダイレクトする。fragment (#) 利用でサーバログ・Referer に残さない。
  2. 新設 `POST /auth/ticket` (body: `{ ticket }`): `GETDEL` で 1 回だけ交換し、`issueTokens(userId)` の結果を返す。
- config 追加: `FRONTEND_URL` (`.env.example` に追記)。
- 受け入れ: ticket の二重交換が 401。frontend `useAuth` フックを新フローに追随させ、ログイン→`/users/me` 取得まで手動確認手順を PR に記載。

#### A1-4. Steam の扱い

- 決定: Steam は OAuth2 ではなく OpenID 2.0 のため現行汎用フローでは動作しない。**本計画では実装せず無効化**する。`config.oauth.steam` を撤去し、`provider=steam` は `INVALID_PROVIDER` を返す。設計書 §3.3 の表から OAuth 扱いを外し「将来対応 (OpenID 2.0 別実装)」と明記。README の機能一覧も同期。
- 受け入れ: `GET /auth/login?provider=steam` が 400。

#### A1-5. `raw_profile` の allowlist 化

- 現状: IdP userinfo 全体を `federated_identities.raw_profile` に平文保存 (設計書 §7 の PII 暗号化宣言と矛盾)。
- 仕様: 保存前に allowlist 抽出関数 `pickProfileFields(provider, userInfo)` を通す。保存対象: `sub`/`id`, `name`/`username`, `picture`/`avatar`, `email`, `email_verified`, `locale` のみ。既存行はマイグレーション `003` で同 allowlist に洗い替える (`UPDATE ... SET raw_profile = jsonb_build_object(...)`)。
- 受け入れ: allowlist 外キー (例: Google の `hd`, `given_name`) が保存されないユニットテスト。

### A2. IdentitySource 抽象化

- 新設 `src/services/identity-sources/` :

```js
// index.js — レジストリ
// 各 source は { key, kind } を持つ。kind により配線先が異なる:
//   kind: "oidc" → /auth/login|/auth/callback|/auth/token のフローに参加
//   kind: "push" → adapter 常駐 (admission push を受けて upsert)
// resolveIdentity の戻り値 (正規化契約):
//   { provider, providerSub, profile: { displayName, email, avatarUrl } }
```

- 既存 Google/Discord は `oidc-source.js` (現 auth.js のフェッチ処理 + `resolveProviderSub`/`resolveProfileData` をここへ移設) として実装。auth.js は「source 選択 → resolveIdentity → federated_identities 検索/作成 → issueTokens」の薄いオーケストレーションに縮小する。
- config 追加: `AUTH_SOURCES` (カンマ区切り、既定 `google,discord`)。列挙されていない source は login も admission も無効。
- 「federated_identities 検索 → 無ければ users 作成 + identity 作成」のロジックが現在 callback と token で重複しているので、`identityService.findOrCreateUser(provider, providerSub, profile)` に一本化する (**1 トランザクションで**。現状は user 作成と identity 作成が別クエリで、identity 挿入失敗時に孤児 user が残る)。
- 受け入れ: 既存 Google/Discord ログインのテストが移設後も green。`AUTH_SOURCES=google` のとき discord ログインが 400。

### A3. CernereSource (kind: "push")

- 依存: `@ludiars/cernere-service-adapter`。**GitHub Packages (private registry) 取得は NPM_TOKEN が要る**ため、Discutere が submodule 化した前例に倣い、取得方法は (a) npm + `.npmrc` か (b) git submodule + `file:` を選べるようにし、CI で動く方を採用して PR に記載する。
- 仕様:
  1. 起動時、`AUTH_SOURCES` に `cernere` が含まれ、かつ `CERNERE_WS_URL`/`CERNERE_SERVICE_SECRET`/`CERNERE_SERVICE_JWT_SECRET` が揃うときのみ `CernereServiceAdapter` を接続する (揃わなければ警告ログ + スキップ。0.2-6)。
  2. `onUserAdmission(user)` → `identityService.findOrCreateUser('cernere', user.id, profile)`。
  3. `onUserRevoke(userId)` → 該当 SID の `revokeAllTokens` + (任意) セッション無効化。
  4. **ログイン経路**: 新設 `POST /auth/cernere` (body: Cernere 発行のサービス token)。adapter の検証機構 (`createServiceAuthMiddleware` 相当) で検証し、`federated_identities(provider='cernere')` から SID を引いて **Voluptas 自身の JWT を発行**して返す。API 側ミドルウェア (`middleware/auth.js`) は無改変 — Voluptas JWT のみを信頼し続ける (0.2-5 の切り離し担保)。
- 未決事項 (実装前に確認、勝手に決めない): Cernere 側のサービス登録 (serviceCode 発行) 手順は Cernere リポジトリ `spec/setup/service-registration.md` に従う。admission push のペイロードに表示名/avatar が含まれるかは adapter の型定義 (`packages/service-adapter/src/types.ts`) を実地確認して合わせる。
- 受け入れ: adapter をモックした統合テストで admission → `/auth/cernere` → `/api/v1/users/me` が通る。`AUTH_SOURCES` から cernere を外して再起動すると `/auth/cernere` は 400、既存ユーザの Google ログインとデータは無傷 (切り離し運用の検証)。

### A4. その他の堅牢化 (小タスク、余力があれば)

- `DELETE /users/me` に再認証 (password は無いので「直近 5 分以内に発行された access_token であること」= `iat` チェック) を追加。
- `helmet` は導入済みだが `express-rate-limit` が auth 系エンドポイントに未配線なら設計書 §7 の値 (ログイン 5 回/分) で配線。

---

## 4. WS-B: sentiment-core パッケージの切り出し

### B1. パッケージ新設 (`Lapilli/packages/sentiment-core` → `@ludiars/sentiment-core`)

- 置き場所の決定: **Lapilli monorepo** (既存 `packages/llm-gateway`/`blackbox` と同居)。Discutere は既に `lib/lapilli` submodule + `file:` 参照の仕組みを持ち、Voluptas も同方式で消費できるため。Lapilli へのアクセスが無い場合は独立リポジトリ `LUDIARS/sentiment-core` で同構成にし、PR に明記する。
- 移植対象 (供給元は Discutere):

| 移植元 | 公開 API | 備考 |
|--------|----------|------|
| `src/flow/sentiment-vector.ts` | `DIM=20`, `VECTOR_SPEC`, `textToVector(text): number[]`, `dot/subtract/norm/cosine/scalarProjection` | TS 版を正本として移植 |
| `src/crawler/sentiment/analyze.mjs` の `features`/`buildVector` | (内部関数) | textToVector と重複するロジックは TS 版に統合。**mjs 側と出力が一致すること** |
| `src/analysis/affect-score.ts` | `scoreText(text): AffectScore` | |
| `src/crawler/sentiment/cascade.ts` | `cascadeSentiment(text, signal, clients)`, `CascadeClients`, `SentimentResult` | LLM クライアントは注入 (パッケージは fetch を直接持たない) |
| `src/crawler/sentiment/lexicon.json` | 同梱データ | |
| `data/affects/vocabulary.json` | `loadAffectVocabulary(): AffectEntry[]` + 同梱データ | 24 語統制語彙 |
| `src/flow/design-gap.ts` の純関数部 | `buildTargetVector(mechanics)`, `computeDesignGap(current, target)` | DB 依存部は移植しない |
| `src/flow/persona-adopt.ts` の純関数部 | `evaluateSpeakers(speakers, opts)`, `weightedMean` | WS-C1 で使用 |

- ビルド: `tsup` 等で CJS+ESM dual、`exports` map 付き。Node >= 20。ランタイム依存ゼロ。
- パッケージ内に README (この表と使用例) を書く。

### B2. Discutere 側の差し替え (バイト等価保証)

- 手順: **先に golden テストを書く**。代表テキスト 50 件 (弾幕調・レビュー調・英語混在を含む) の `textToVector`/`scoreText`/`cascadeSentiment(Tier0)` の出力を JSON に固定 → パッケージ参照へ差し替え → golden が一致することを確認。`data/games/*.sentiment.json` の再生成 1 件で `game_vector` が一致することも確認。
- 差し替え箇所: `src/flow/sentiment-vector.ts` / `src/analysis/affect-score.ts` / `src/crawler/sentiment/cascade.ts` を re-export shim にする (import 元パスを一斉変更しない。既存 import はそのまま生かす)。
- `lexicon.json`/`vocabulary.json` の**正本はパッケージ側へ移し**、Discutere の `data/affects/vocabulary.json` は seed スクリプトが参照するため当面シンボリック的に残す (二重化する場合は CI で diff チェックを足す)。
- 受け入れ: `npm run typecheck` + `test:crawler`/`test:core` green + golden 一致。**Discutere の挙動変更ゼロ**がこのタスクの成功条件。

---

## 5. WS-C: 嗜好・ペルソナ分析の統合 (Voluptas)

### C1. sentiment-core 組込みと発話プロファイル

- Voluptas に `@ludiars/sentiment-core` を導入 (CJS require)。
- 新設 `src/services/affectProfile.js`: ユーザの自由記述 (アンケート freetext 回答、および将来の発話ログ) を `textToVector` でベクトル化し、`evaluateSpeakers` 互換の入力に整形して**エンゲージメント重み付き 20 次元 affect プロファイル**を計算する。
- 新テーブル (migration 004): `player_affect_profiles(user_id UUID PK FK, vector FLOAT8[] /*len20*/, sample_texts INTEGER, vector_spec_version INTEGER, computed_at TIMESTAMPTZ)`。
- 入力が 0 件のユーザは行を作らない (ゼロベクトルを「無感情」と誤読させない)。

### C2. アンケートの 15 軸スコアリング

- Voluptas `surveys.questions[]` の各設問に**任意フィールド `axis`** を追加 (値は Discutere `PREFERENCE_AXES` のキー文字列、例 `mtg.timmy`, `style.explorer`)。既存 `dimension` (12 次元用) と併存し、両方付けてよい。
- 回答提出時、`axis` 付き設問を Discutere `persona-questionnaire.ts` と同じ規則 (scale 正規化 → 軸スコア加算) でスコアし、新テーブル `player_preference_axes(user_id, axis VARCHAR(64), score FLOAT8, samples INTEGER, updated_at)` (PK: user_id+axis) に upsert する。
- **12 次元との写像は作らない** (0.2-2)。スコアリング規則は Discutere の該当ファイルを読んで同一に実装し、出典コメントでファイル名を示す。

### C3. 統合プロファイル API

- `GET /api/v1/analysis/me` のレスポンスを拡張 (後方互換: 既存キーは不変):

```json
{
  "ok": true,
  "data": {
    "preference": { "dimensions": [...12], "vector": [...], "classification": {...}, "tags": [...] },
    "preferenceAxes": { "mtg.timmy": 0.7, "style.explorer": 0.4 },   // C2。無ければ {}
    "affectProfile": { "vectorSpecVersion": 1, "vector": [...20], "sampleTexts": 12 },  // C1。無ければ null
    "experimental": { "subtypes": {...} }   // C5 で移設
  }
}
```

### C4. 疑似 ID 境界 (Voluptas → Discutere)

- 新設 `src/services/pseudoId.js`: `pseudoId(sid) = "ext:voluptas:" + hmacSha256(sid, VOLUPTAS_PSEUDO_ID_SECRET).slice(0, 16)`。secret は env 必須 (未設定なら機能自体を無効化)。
- Voluptas から Discutere へ発話・テキストを渡す将来経路 (本計画ではエクスポート CLI `scripts/export-utterances.js` のみ実装: 出力 JSON は Discutere `ExternalUtterance` 形式、`authorId` に疑似 ID) はすべてこの関数を通す。**SID/email/IdP sub を出力に含めない**テストを付ける。
- `DELETE /users/me` のドキュメントに「Discutere へ渡った疑似 ID データは削除範囲外 (仮名化済み)」と明記する — 法務判断が必要なら未決事項として報告。

### C5. analysisEngine の誠実化

- `detectSubtypes` (`idx % length` の機械割当てで意味を持たない) を API 応答の一級市民から外し、`experimental.subtypes` へ移設。コード上に「較正されるまで実験的」とコメント。
- `EVENT_DIMENSION_MAP` の `session_heartbeat` → `story_banal 0.3` / `gamer_timmy 0.2` を削除 (放置プレイが人格判定を動かすため)。heartbeat は Intensity 系の集計 (セッション時間) にのみ使う。
- `choice` 型設問の一律 +5 を、選択肢に `weight` があればそれを使い、なければ従来値、に変更。
- 受け入れ: 既存ユニットテスト更新 + `POST /analysis/me` の後方互換 (既存キーが残る) テスト。

---

## 6. WS-D: プレイ動画ベースの感情タイムライン

### 設計方針 (レビュー §5 の確定事項)

- 観測ソースは**タイムスタンプ付き視聴者コメント** (niconico 弾幕 vpos / YouTube ライブチャット offset)。プレイヤー表情・映像解析・ASR は**やらない** (§8 非目標)。
- 観測が測るのは「視聴者の反応」であり「プレイヤーの感情」ではない。API・UI の文言でも `viewer reaction` と呼び、混同させない。
- 面白さは瞬時 joy ではなく**ビート単位の「意図 affect との一致度」**で表す。コメント密度は arousal の代理指標として曲線に併記する。

### D1. Discutere コレクタの動画内時刻保持 (Discutere 側・最小変更)

現状の欠落 (実地確認済み):

- `src/crawler/sources/types.ts` の `ExternalUtterance` に動画内時刻フィールドが**無い**。
- `src/crawler/sources/niconico.ts` の `NicoComment` は `vposMs` を取り込んでおらず (nvComment API のレスポンスには存在する)、`mapNicoComment` も捨てている。
- `src/crawler/sources/youtube-livechat.ts` は `offsetMs` を持っているが `sourceUrl` の `&t=` に焼き込むだけで構造化フィールドが無い。

仕様:

1. `ExternalUtterance` に **任意フィールド `videoOffsetMs?: number`** を追加 (0 起点、動画内経過 ms)。任意フィールドの追加なので既存 source・importer は無改変で通る (typecheck で確認)。
2. `NicoComment` に `vposMs?: number` を追加し、nvComment レスポンスから取り込み、`mapNicoComment` で `videoOffsetMs` に写す。
3. `youtube-livechat.ts` の `mapLiveChatRenderer` で `videoOffsetMs: offsetMs` を設定する。
4. importer (KG 取り込み) 側は当面 `videoOffsetMs` を**保存しなくてよい** (Voluptas 向けエクスポートで使うのが主目的)。保存する場合は utterances サイドカー列として別タスクに切る。
- 受け入れ: `test:crawler` green。niconico/livechat の map 関数ユニットテストに offset 検証を追加。既存 golden (あれば) 不変。

### D2. 観測タイムラインの取り込みと集約 (Voluptas)

- 新テーブル (migration 005):

```sql
CREATE TABLE affect_timelines (
  id            BIGSERIAL PRIMARY KEY,
  game_id       VARCHAR(100) NOT NULL,
  source_kind   VARCHAR(30)  NOT NULL,  -- 'video_comments' | 'play_session'(D5)
  source_ref    VARCHAR(200) NOT NULL,  -- 動画ID or session_id
  bin_ms        INTEGER      NOT NULL,  -- 集約ビン幅 (既定 30000)
  series        JSONB        NOT NULL,  -- [{ t: <bin開始ms>, vector: [..20], n: <コメント数>, density: <n/bin> }]
  vector_spec_version INTEGER NOT NULL DEFAULT 1,
  algo_version  INTEGER      NOT NULL DEFAULT 1,
  created_at    TIMESTAMPTZ  NOT NULL DEFAULT now(),
  UNIQUE (source_kind, source_ref, bin_ms, algo_version)
);
```

- 新サービス `src/services/affectTimeline.js`:
  - 入力: `ExternalUtterance[]` 形式 JSON (D1 の `videoOffsetMs` 付き。取得は Discutere 側 crawl の出力ファイル、または新設 CLI `scripts/import-timeline.js <file>` で投入)。
  - 処理: `videoOffsetMs` 有りのみ対象 → bin (既定 30 秒) ごとに `textToVector` の**平均** + `n`/`density` を算出 → `series` に保存。ビン内 0 件は要素を作らない (欠測と 0 を区別)。
  - 全て純関数 (`aggregateTimeline(utterances, binMs)`) + 保存の 2 層に分け、純関数側をユニットテスト。
- API: `GET /api/v1/games/:gameId/timelines` (一覧)、`GET /api/v1/timelines/:id` (series 本体)。Bearer 必須。
- 既知の未知数 (実装時に必ず計測して PR に書く): 弾幕特有表現 (「草」「8888」「!?」等) の lexicon カバレッジ。ヒット率 (`textToVector` が非ゼロを返す割合) を集約時に `series` メタとして記録し、著しく低い (目安 3 割未満) 場合は課題報告する。lexicon への語彙追加は sentiment-core 側の別 PR とする (Discutere と共有されるため単独で変更しない)。

### D3. ゲーム展開フォーマット「ビートスクリプト」と目標曲線

- 新テーブル (migration 005 に同居):

```sql
CREATE TABLE game_beat_scripts (
  game_id   VARCHAR(100) NOT NULL,
  version   INTEGER      NOT NULL,
  beats     JSONB        NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  PRIMARY KEY (game_id, version)
);
```

- `beats` スキーマ (JSON Schema でバリデーション。`intended_affect` の値は sentiment-core 同梱 `vocabulary.json` の `key` に限定):

```json
[
  { "beat": "tutorial",   "order": 1, "intended_affect": ["discovery"],
    "intended_intensity": 0.3, "markers": { "note": "レベル1-3" } },
  { "beat": "first_boss", "order": 4, "intended_affect": ["tension_release"],
    "intended_intensity": 0.9, "markers": { "t_hint_ms": [600000, 780000] } }
]
```

- `markers.t_hint_ms` は動画タイムラインとの対応付け (アラインメント) の**手動指定** (開始/終了 ms)。**自動アラインメントは実装しない** — 本計画ではビートと時刻の対応は人手入力 (API/JSON) とする。将来の半自動化 (コメント密度の山谷提案) は非目標ではないが本計画外。
- 目標曲線: `buildTargetVector` (sentiment-core) をビート単位に適用し `target(t)` を得る純関数 `beatScriptToTargetSeries(beats, binMs)` を実装。`intended_affect` key → vocabulary の valence/emotion 対応は Discutere `design-gap.ts` の `buildTargetVector` の規則を踏襲する。
- API: `PUT /api/v1/games/:gameId/beat-script` (新 version 追加、破壊更新しない) / `GET .../beat-script` (最新)。

### D4. 時系列 DesignGap API

- `GET /api/v1/games/:gameId/timelines/:id/gap?beatVersion=N`:
  - `series` の各ビンに対し、`t_hint_ms` で対応するビートの target ベクトルとの差分 `gap = target − observed` (sentiment-core `computeDesignGap`) を計算。
  - ビート単位の集約: 各ビートについて `{ beat, matchScore, gapTop: [{dim, value}...3], bins: n }` を返す。`matchScore` は観測平均ベクトルと target の cosine (sentiment-core の `cosine`)。
  - ビートに `t_hint_ms` が無い区間は `unaligned` として除外リストに明示 (黙って捨てない)。
- これが「面白さのベクトル」の一次出力である: **面白さ = ビートごとの意図一致度 (matchScore) + 不一致の内訳 (gapTop)**。単一スカラーの「面白さ曲線」は出さない (レビュー §5.4 の判断)。

### D5. 計装ゲームのログ同期タイムライン (M4 / 依存: Ludellus tuning-log 実装)

- 前提: `ludellus-tuning-log-design.md` の migration 003 (`content_items` / `player_ability_snapshots` / `player_tuning_params`) と `trial_result` イベントの実装。これ自体は同設計書 §12 のフェーズに従い**別トラック**で進む。
- 本タスク: `play_events` の `mono_ms` 系列を D2 と同じ `affect_timelines(source_kind='play_session')` に写像する。感情ベクトルではなくイベント密度・成績 (正答率/リトライ) を series に持たせ、D4 で「ゲーム展開の実測タイムライン」としてビートアラインメントの**厳密な根拠**に使う (`markers.t_hint_ms` の人手入力を置き換える)。
- 録画同期: セッション開始イベントに `recording_started_at` (壁時計) を任意で持たせるだけでよい (設計書 §5 の metadata に 1 キー追加)。
- 詳細仕様は M3 完了後に確定するため、本計画では**テーブルを流用できる形にしておくこと**のみが要求 (D2 の `source_kind` 列が既にその布石)。

---

## 7. テスト・検証の全体要求

| 対象 | 要求 |
|------|------|
| Voluptas | `npm test` (node --test)。新規コードはサービス層を純関数化してユニットテスト。DB 依存テストは `pg` をモックするか、既存パターンに従う (既存テストが無いモジュールを触る場合、最低限そのモジュールのテストを新設) |
| Discutere | `npm run typecheck` + 触った領域の `test:*` ランナー。WS-B は golden テスト必須 |
| sentiment-core | パッケージ内に vitest 等でユニットテスト + Discutere golden との一致テスト |
| E2E 手動確認 | 各 PR の説明に「手動確認手順」(curl 例) を書く。特に A3 (Cernere 有/無 両モード) と D2〜D4 (実在動画 1 本での曲線生成) |

---

## 8. 非目標 (実装しないこと)

1. **表情ベースの感情推定** (facecam 解析) — 科学的妥当性が弱く、どのフェーズにも入れない (レビュー §5.1)。
2. **任意の市販ゲーム映像からの自動展開認識** (VLM フレーム分類 / OCR / シーン検出) — 研究グレード。アラインメントは人手 `t_hint_ms` と D5 のログ同期のみ。
3. **音声認識 (ASR) による実況テキスト化** — 将来の実験枠 (P4)。基盤コードを先回りで作らない。
4. **OIDC OP の自作** (A1-2 で看板を下ろす決定済み)。
5. **12 次元⇄15 軸⇄20 次元の相互写像**。
6. **Discutere KG への Voluptas 個人データ取り込み**。
7. **生理指標 (心拍等) 連携**。

---

## 9. リスクと未決事項 (実装担当が着手前に確認すべきこと)

| # | 項目 | 対処 |
|---|------|------|
| R1 | 弾幕語彙の lexicon カバレッジ不足 | D2 でヒット率を計測しメタ保存。低ければ課題報告 (勝手に lexicon を膨らませない) |
| R2 | Cernere のサービス登録手順・admission ペイロードの実形 | Cernere `spec/setup/service-registration.md` と `service-adapter` の型定義を実地確認。合わなければ A3 を止めて報告 |
| R3 | Lapilli への書き込み権限 | 無ければ独立リポジトリ `sentiment-core` に切替 (B1 に記載) |
| R4 | `@ludiars` private registry (NPM_TOKEN) | submodule + `file:` 方式を第一候補に (Discutere の前例) |
| R5 | 疑似 ID の GDPR 上の扱い (削除権の範囲) | C4 のドキュメント記載に留め、確定は人間の判断に委ねる |
| R6 | YouTube ライブチャットリプレイの quota コスト | Discutere `youtube-quota.ts` の管理下で実行。大量取得ジョブを無制限に回さない |
| R7 | `play_events` 90 日保持と再計算可能性の矛盾 | `affect_timelines` は派生テーブルとして長期保持。生コメント JSON はエクスポートファイルとして保管 (バックアップ対象への追加は Discutere `src/backup/` の枠で別途) |

---

## 10. Definition of Done (計画全体)

- [ ] M1: Voluptas auth の A1-1〜A1-5 が閉じ、`@ludiars/sentiment-core` が Discutere で golden 一致のまま稼働
- [ ] M2: `AUTH_SOURCES=cernere,google` と `AUTH_SOURCES=google` の両モードで全 API が動作し、`GET /analysis/me` が 3 空間併記で返る
- [ ] M3: 実在動画 1 本以上について、観測タイムライン→ビートスクリプト→Gap API が end-to-end で動き、README に手順が載る
- [ ] M4: 計装セッション由来の `source_kind='play_session'` タイムラインが生成できる
- [ ] 全 PR がレビュー可能な粒度 (1 タスク 1 PR) で、各 PR に手動確認手順が記載されている
- [ ] 設計書 (`player-profile-backend-design.md`) の記述が実装と一致するよう改訂されている (A1-2, A1-4, §7 の未実装項目の明記)

---

## 改訂履歴

| バージョン | 日付 | 内容 |
|------------|------|------|
| 1.0 | 2026-07-12 | 初版 (レビュー文書 v1 の確定判断を仕様化) |
