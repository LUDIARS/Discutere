# Voluptas × Discutere 統合 — 設計・実現可能性レビュー

> 2026-07-12 | 対象: [LUDIARS/Voluptas](https://github.com/LUDIARS/Voluptas) (player-profile-server)
> レビュー観点: (1) Cernere 認証基盤の導入 (プロバイダ差し替え可能)、(2) Discutere 実装による
> ゲーム嗜好・ペルソナ分析、(3) プレイ動画ベースのタイムライン感情曲線と面白さの定量化 — 特に (3) を重点評価。

---

## 0. 結論サマリ (正直な総合判定)

| 項目 | 判定 | 一言 |
|------|------|------|
| (1) Cernere 認証 + プロバイダ抽象化 | **実現可能・妥当** | Voluptas は既に federated_identities で多 IdP 前提。Cernere を「もう 1 つの identity source」として抽象境界に載せるのが正着。ただし現行 auth 実装に修正必須の穴が複数ある (§3.3) |
| (2) Discutere 転用の嗜好・ペルソナ分析 | **部分的に有効** | 「テキスト→20次元感情ベクトル」「話者採用 (persona-adopt)」「アンケートスコアリング」は切り出して転用できる。ただし Voluptas の 12 次元と Discutere の 20 次元 + 15 軸は**別物**で、無理に統一しないのが正解 (§4.2)。匿名 workspace 方針との境界設計が必須 (§4.4) |
| (3) 動画ベース感情曲線 | **構成次第で二極化** | 「視聴者コメント時系列 × 既存 20 次元辞書 × ビート単位の感情スクリプト」なら**実現可能性が高く、Discutere 資産をほぼそのまま使える**。一方「プレイヤーの表情/映像からの直接感情推定」と「任意の市販ゲーム映像からの自動展開認識」は**研究グレードで MVP に入れるべきでない** (§5) |

最重要の設計判断は 2 つ:

1. **感情空間は Discutere の固定 20 次元を共通正本にする** (Voluptas の 12 次元嗜好ベクトルとはレイヤを分けて併存させ、写像しない)。
2. **(3) の入力は「プレイヤーの生体/表情」ではなく「タイムスタンプ付き視聴者コメント (niconico vpos / YouTube ライブチャット) + 計装ゲームのプレイログ」から始める**。前者は科学的妥当性が弱く、後者は既存資産で今すぐ動く。

---

## 1. レビューの前提 — 三者の資産マップ

### 1.1 Voluptas (player-profile-server) の現状

- **認証**: 自前ミニ IdP。Google/Discord/Steam を `federated_identities (provider, provider_sub)` で SID (UUID v4) に連合。RS256 JWT + JWKS 配信 + refresh rotation (`tokenService.js` — ハッシュ保存・ローテーションは正しく実装済み)。
- **嗜好分析** (`analysisEngine.js`): 12 次元ベクトル = Gamer Pattern (MTG 心理 5: timmy/johnny/spike/vorthos/melvin) + Mechanics Pattern (Caillois 4: agon/alea/ilinx/mimicry) + Story Dynamics (Berne 3: winner/banal/loser)。入力は `play_events` の `EVENT_DIMENSION_MAP` (手置き重み) + アンケートのリッカートスコア。
- **プレイログ**: `play_sessions` / `play_events` (JSONB)。加えて `ludellus-tuning-log-design.md` が**トライアル粒度 + 単調クロック計測**の高品質なログ設計を定義済み (未実装、migration 003 相当)。
- **未実装/宣言倒れ**: 設計書 §7 の「PII カラムレベル暗号化」は実装なし (`raw_profile` JSONB に IdP 生プロファイルを平文保存)。分析ジョブは「非同期 + MQ」設計だが実体は同期 `POST /analysis/me`。

### 1.2 Discutere の転用可能資産 (実在確認済み)

| 資産 | 実体 | 転用先 |
|------|------|--------|
| **固定 20 次元感情ベクトル空間** | `src/crawler/sentiment/analyze.mjs` `VECTOR_SPEC` (valence/arousal + Plutchik8 + アスペクト8 + meta2)、TS 版 `src/flow/sentiment-vector.ts` `textToVector` — **辞書ベース・決定論的・外部 API 不要** | (2)(3) の感情ベクトル化エンジン |
| **0+1 感情カスケード** | `src/crawler/sentiment/cascade.ts` — Tier0 辞書 → Tier1 ローカル LLM → residual メイン LLM | 辞書で拾えない口語の補完 |
| **Affect 統制語彙** | `data/affects/vocabulary.json` (24 語、`key` + valence + MDA)、`intended_affect` と観測 affect の key 完全一致 Gap 判定 | (3) の「意図された感情」記述 |
| **DesignGap = ベクトル差分** | `src/flow/design-gap.ts` `buildTargetVector(mechanics)` / `computeDesignGap(current, target) = target − current` | (3) の時系列版 Gap にそのまま拡張可能 |
| **ゲーム仕様フォーマット** | `data/games/*.md` frontmatter: `mechanics[]{name, description, intends, intended_affect}` + `enrichMechanics` (LLM 増補) | (3) のゲーム展開フォーマットの母体 |
| **感情曲線の先行実装** | `data/games/*.sentiment.json` の `sentiment_curve` (**月次**の感情推移) | (3) の曲線データモデルの雛形 (時間軸を動画内時刻に替える) |
| **タイムスタンプ付きコメント収集** | `src/crawler/sources/niconico.ts` (**nvComment 弾幕 = 動画内時刻 vpos 付き**)、`youtube-livechat.ts`、`youtube-comments.ts`、`steam.ts` | (3) の観測データソース |
| **話者採用 (実在ユーザ→ペルソナ)** | `src/flow/persona-adopt.ts` `evaluateSpeakers` (純粋関数、エンゲージメント重み付き affect 平均、採用条件付き) | (2) のプレイヤープロファイル生成 |
| **嗜好 15 軸アンケートスコアリング** | `src/flow/persona-questionnaire.ts` `PREFERENCE_AXES` (mtg.timmy/johnny/spike + Bartle 系 12) | (2) のアンケート統合 |

**Discutere に無いもの (誤解しやすい点)**: 動画の音声認識・映像解析は一切ない。niconico/YouTube コレクタはコメント/メタデータのみ。「動画を解析する」機能はゼロから作ることになる。

### 1.3 Cernere の統合インターフェース

- `@ludiars/cernere-service-adapter` の **User admission** (`CernereServiceAdapter`): Cernere が承認したユーザを WS push で受け、サービス側が自前 DB へ upsert + サービス固有 token を発行するモデル。`createServiceAuthMiddleware` 付き。
- `packages/id-service`: サービス側に組み込む Identity SDK (Repo 注入型)。
- **注意**: Cernere は OIDC IdP (authorization code endpoint を外部サービスに提供する形) ではなく、**admission push + service token** モデル。Voluptas の既存 PKCE/OIDC フローとはプロトコルが噛み合わないため、「provider の 1 つとして `/auth/login?provider=cernere`」という素朴な追加はできない (§3.2)。

---

## 2. 全体アーキテクチャ提案

```
                        ┌────────────── Cernere (任意) ──────────────┐
                        │  admission push / service token            │
                        ▼                                            │
┌──────────┐   Identity Source 抽象境界   ┌─────────────────────────┐
│ Google / │ ─ OIDC (直連合・従来通り) ─▶ │ Voluptas                │
│ Discord  │                              │  users / federated_ids  │
└──────────┘                              │  play_sessions/events   │
                                          │  preference_vector(12d) │
                                          │  ★affect_timeline(20d)  │← 新設 (3)
                                          │  ★game_beat_script      │← 新設 (3)
                                          └───────────┬─────────────┘
                                                      │ 疑似ID (ext:voluptas:<hash>)
                                                      │ テキスト/ログの分析依頼
                                          ┌───────────▼─────────────┐
                                          │ sentiment-core (切出し)  │
                                          │  20次元辞書ベクトル化     │
                                          │  affect 統制語彙         │
                                          │  cascade (辞書→LLM)     │
                                          └───────────▲─────────────┘
                                                      │ 同一パッケージを共用
                                          ┌───────────┴─────────────┐
                                          │ Discutere (匿名 workspace)│
                                          │  KG / persona / 議論     │
                                          └─────────────────────────┘
```

- **sentiment-core の切り出し**が統合の要。`analyze.mjs` + `sentiment-vector.ts` + `affect-score.ts` + `lexicon.json` + `vocabulary.json` は依存がほぼゼロで、Discutere が既にやっている Lapilli/Canalis 型の **git submodule + `file:` 参照**パターンにそのまま載る。Voluptas (CommonJS/Express) からも Discutere からも同一実装を参照でき、感情空間の二重定義を防ぐ。
- Discutere 本体 (persona-engine、KG、議論フロー) は**切り出さない**。重量級で Discord/SQLite に絡んでおり、Voluptas が欲しいのは分析関数であってチャットボットではない。

---

## 3. (1) 認証 — Cernere 導入とプロバイダ差し替え

### 3.1 評価: 方針は妥当

Voluptas は最初から `federated_identities` で多 IdP を前提にしており、「Cernere はオプション、切り離し運用では Google 等の直連合」という要求と構造的に相性が良い。**SID (Voluptas 発行 UUID) を全データの主キーに保ち続ける**限り、認証ソースを後から足し引きしてもプロファイル・ログ・分析結果は無傷で残る。これが切り離し運用の担保になる。

### 3.2 推奨設計: Identity Source 抽象境界

Cernere の admission push と OIDC の redirect flow はプロトコルが異なるため、`provider` 値の追加ではなく **1 段上の抽象**で吸収する:

```
IdentitySource (interface)
├── OidcSource      — 既存 Google/Discord (authorization code → userinfo → sub)
├── CernereSource   — CernereServiceAdapter (admission push → upsert / revoke push → 失効)
└── (SteamSource)   — OpenID 2.0 (§3.3-d 参照、OAuth2 ではない)
```

- どのソースも最終的に `resolveIdentity() → { provider, provider_sub, profile }` に正規化し、既存の「federated_identities 検索 → 無ければ SID 発行」ロジック (auth.js の callback 処理) に合流させる。Cernere は `provider='cernere'`, `provider_sub=<cernere user id>` の 1 行になる。
- **トークンは常に Voluptas 発行の RS256 JWT に統一**する (Cernere のサービス token をそのまま API 認証に使わない)。API ミドルウェア (`middleware/auth.js`) は無改変で済み、切り離し運用時にコード差分が生じない。
- **モード切替は config**: `auth.sources = ["google", "discord"]` / `["cernere", "google"]` のような宣言。Cernere 有効時も直連合を殺す必要はない (併存可)。
- **アカウントリンク**: email 一致による自動リンクは**やらない** (IdP 間の email 検証レベル差による乗っ取り経路)。既存の明示リンク API (`POST /users/me/identities`) に統一。
- **Discutere との関係**: Discutere は現行どおり Discord 認証境界のまま (CLAUDE.md の canonical 方針を変えない)。ユーザ単位の突合は Voluptas 側の `federated_identities(provider='discord')` が持つ Discord ID で可能であり、Discutere に認証を持ち込む必要はない。サービス間通信が要る場合は Cernere PeerAdapter か静的サービストークンで S2S として扱う。

### 3.3 現行 auth 実装の修正必須事項 (Cernere 以前の問題)

正直に言うと、現行実装は「OIDC 型」を名乗るには穴がある。Cernere 統合より先に直すべき:

- **(a) `stateStore` が in-memory の `Map`** (`auth.js:15`)。設計書 §8 は「水平スケール可能なステートレス設計」を謳うが、複数インスタンスで即壊れる。Redis へ移す (設計書に Redis は既にある)。
- **(b) PKCE が飾りになっている**。`/auth/login` で `code_challenge` を保存するが**誰も検証していない**。`/auth/token` は `code_verifier` を IdP に横流しするだけで、Voluptas 自身は自前の authorization code を発行していない。つまり「OpenID Connect 型の ID 発行サービス」ではなく「IdP プロキシ」。どちらかに倒す: (i) 自前 code 発行 + 自前 PKCE 検証まで実装して本物の OP になる、(ii) 設計書から OIDC OP の看板を下ろし「IdP フェデレーション + 自前 JWT」と正しく書く。**推奨は (ii)** — OP を自作する動機が現状ない。
- **(c) `/auth/callback` が JSON でトークンを返す**。callback はブラウザの redirect で着地する URL なので、JSON 返却では SPA がトークンを受け取れない (フロントへの受け渡し経路が成立していない)。フロントの redirect 完結フローに直すか、`/auth/token` 側に一本化する。
- **(d) Steam は OAuth2 ではない**。Steam の個人認証は OpenID 2.0 であり、`authorizationUrl`/`tokenUrl`/`userinfoUrl` を前提とする現行の汎用フローでは**動かない**。設計書 3.3 の表からも「OAuth」扱いを外し、`SteamSource` として別実装するか Phase を切る。
- **(e) `raw_profile` に IdP 生プロファイルを平文保存**。設計書 §7「PII はカラムレベル暗号化」と矛盾。最低限、保存フィールドを allowlist (sub/name/picture) に絞るか、暗号化を実装するまで raw 保存をやめる。
- (f) Cernere 側思想との整合: Cernere は「破壊的操作は常時接続セッションからのみ」という強いモデルを持つ。Voluptas の REST + Bearer はそこまで求められていないが、`DELETE /users/me` (全データ削除) だけは再認証 or Cernere 経由確認を挟む価値がある。

**工数感**: 抽象境界の導入 + CernereSource + (a)〜(c) の修正で 2〜3 週間相当。設計書のマイルストーン Phase 1 の枠内に収まる規模で、実現可能性に疑義はない。

---

## 4. (2) Discutere 実装による嗜好・ペルソナ分析

### 4.1 評価: 「そのまま使う」ではなく「3 部品を切り出して使う」

Discutere はアプリであってライブラリではない。転用価値が高く、かつ切り出しコストが低いのは次の 3 部品:

1. **sentiment-core** (§2) — テキスト → 20 次元。Voluptas に入ってくる自由記述 (アンケート freetext、Discord 発言、レビュー) を即ベクトル化できる。決定論的・無コストで、LLM は cascade の Tier1/2 としてオプション。
2. **`evaluateSpeakers` / `adoptPersonas`** (`persona-adopt.ts`) — 発話群 → エンゲージメント重み付き affect プロファイル + 採用判定。純粋関数なので移植は容易。Voluptas では「ユーザの発話履歴 → affect プロファイル」として使える。
3. **`persona-questionnaire` のスコアリング** — Voluptas の `surveys` と Discutere の質問票は同型 (scale + dimension 写像)。設問スキーマを共通化すれば、Voluptas のアンケート回答を Discutere の 15 軸 (`PREFERENCE_AXES`) でもスコアできる。

### 4.2 次元体系の衝突 — 統一しないことを推奨

現状 3 つの嗜好/感情空間が並立している:

| 空間 | 次元 | 入力 | 意味 |
|------|------|------|------|
| Voluptas `preference_vector` | 12 (MTG5 + Caillois4 + Berne3) | プレイイベント + アンケート | 行動から見た嗜好 |
| Discutere `PREFERENCE_AXES` | 15 (mtg 3 + Bartle 系 12) | アンケート質問票 | 質問応答から見た嗜好 |
| Discutere 20 次元 affect | 20 (Plutchik8 + アスペクト8 + …) | テキスト | 発話に表出した感情 |

MTG 心理タイプが Voluptas は 5 種 (vorthos/melvin あり)、Discutere は 3 種、と**同名でも互換性がない**。ここで無理に単一空間へ統一すると両プロダクトの既存データとコードが壊れる。推奨:

- **感情 (affect) は Discutere 20 次元を共通正本に** — Discutere 側は「新空間は作らない」が設計原則 (`sentiment-vector.ts` 冒頭) であり、Voluptas がこれに乗るのが摩擦最小。
- **嗜好 (preference) は各プロダクトの空間を維持**し、統合プロファイル API (`GET /analysis/me`) は両ベクトルを**併記**する。写像 (12d⇄15d) が必要になったら分析時にバージョン付き変換として実装し、保存データには焼き込まない。

### 4.3 Voluptas 分析エンジン自体への正直な指摘

転用の前提として、現行 `analysisEngine.js` の品質を正しく認識しておくべき:

- `EVENT_DIMENSION_MAP` の重みは全て手置きで較正なし。`session_heartbeat` (定期パルス) が `story_banal 0.3` に写像されており、**放置プレイが人格判定を動かす**。
- `detectSubtypes` はサブタイプ配列に対し `idx % mechanicsValues.length` で機械的にスコアを割り当てており、**サブタイプ判定に実質的な意味がない** (擬似分析)。API から返すならラベルを「実験的」と明示するか、いったん落とすのが誠実。
- `choice` 型設問は回答内容に関わらず一律 +5。
- つまり現行 12 次元ベクトルは**ヒューリスティックなプレースホルダ**であり、これを根拠に難易度チューニング等を自動化するのは時期尚早。対照的に `ludellus-tuning-log-design.md` (トライアル粒度・較正ループ・閉ループ評価) は方法論的に堅牢で、**能力推定はそちらの設計を正とし、嗜好 12 次元は「表示用プロファイル」に格下げして混同しない**ことを勧める。

### 4.4 プライバシー境界 (必須の設計制約)

Voluptas は実名級 PII (email、IdP プロファイル) を持つ。Discutere は**匿名 workspace が canonical** (個人アンカーはマスクし `論者#xxxx` 表示、KG に編集者名を保存しない)。この 2 つを繋ぐときの規約:

- Voluptas → Discutere (分析依頼・発話持ち込み) は **SID を不可逆ハッシュ化した疑似 ID (`ext:voluptas:<hash>`)** のみで渡す。Discutere の既存話者アンカー形式 (`ext:<source>:<authorId>`) にそのまま載る。
- Discutere → Voluptas (分析結果の還元) は本人 opt-in がある場合に限る。
- `DELETE /users/me` の削除範囲に「Discutere 側へ渡った疑似 ID の発話」を含めるかは事前に決めて規約に書く (ハッシュが不可逆でも、GDPR 的には仮名化データは個人データでありうる)。

**判定: 実現可能。工数は sentiment-core 切り出し 1 週間 + Voluptas 側配線 1〜2 週間程度。最大のリスクは技術ではなく、次元体系を安易に統一して両側の既存資産を壊すこと。**

---

## 5. (3) プレイ動画ベースの感情曲線 — 重点レビュー

要求を成分分解する。「特定ゲームのプレイ動画をベースにタイムラインで感情曲線を記述し、ゲーム展開を仕様からフォーマット化し、ユーザー反応と面白さのベクトルを定量化する」は 4 つの独立した問題であり、**実現可能性が成分ごとに大きく異なる**。

### 5.1 成分 A: 感情の観測ソース — どこから「感情」を取るか

| 案 | 実現可能性 | 評価 |
|----|-----------|------|
| **A-1. タイムスタンプ付き視聴者コメント** (niconico 弾幕 vpos / YouTube ライブチャット offset) | **高 — 既存資産で即動く** | Discutere の `niconico.ts` は既に nvComment (動画内時刻付き弾幕) を取得しており、`youtube-livechat.ts` も同様。これを既存 20 次元辞書ベクトル化 (`textToVector`) に通して時間ビンで集約すれば、**クラウドソースの感情曲線が新規研究ゼロで作れる**。多数の視聴者の集約なので n=1 ノイズにも強い |
| A-2. プレイヤーの表情 (facecam) | **低 — 研究グレード、科学的妥当性に難** | 表情→内的感情の対応は文脈依存で弱いというのが心理学の現在地 (Barrett らのレビュー以降、表情ベース感情推定への風当たりは強い)。特に「面白い」はフロー状態では無表情になり、**面白さの検出に最も向かない**。Azure Face API の感情推定が廃止された経緯も同根。笑い声・悲鳴などの離散イベント検出に割り切るなら可 |
| A-3. プレイヤーの音声 (実況の発話・笑い) | 中 | ASR (Whisper 等) → テキスト → 既存 20 次元、は技術的に素直で、A-1 と同じベクトル空間に落ちる。笑い・叫びの音響イベント検出も比較的堅い。ただし Discutere/Voluptas とも音声処理基盤はゼロからで、実況スタイルに依存する |
| A-4. 生理指標 (心拍等) | 中 (精度) / 低 (配布) | 妥当性は最も高いがデバイス前提が配布ハードル。スコープ外を推奨 |

**推奨: Phase 1 は A-1 一本。** 注意点として、A-1 が測るのは「プレイヤーの感情」ではなく「**視聴者の反応**」である。ゲームの面白さ評価という目的にはむしろ視聴者反応の方が母数が大きく実用的だが、「プレイヤー体験の感情曲線」と混同して報告しないこと (ネタコメント文化・盛り上がり箇所にしかコメントが付かない密度バイアスも補正が要る — コメント密度そのものを arousal の代理指標として曲線に併記するのが実務的な対処)。

### 5.2 成分 B: ゲーム展開のフォーマット化 — 「感情スクリプト」

Discutere に静的な意図フォーマットは既にある: `data/games/*.md` の `mechanics[]{name, intends, intended_affect}` + 統制語彙 24 語 + `buildTargetVector` (mechanics → 20 次元 target)。**無いのは時間軸**。これをメカニクス単位から「ビート (展開区間)」単位へ拡張する:

```yaml
# game_beat_script (新設・md frontmatter 拡張 or Voluptas 新テーブル)
beats:
  - beat: tutorial        # 展開区間の種別
    order: 1
    intended_affect: [discovery, relief]   # vocabulary.json の key
    intended_intensity: 0.3
    markers: { level_range: [1, 3] }        # ログ/映像との対応付け手がかり
  - beat: first_boss
    order: 4
    intended_affect: [tension_release]
    intended_intensity: 0.9
```

- `intended_affect` は既存 `vocabulary.json` の key をそのまま使う (`tension_release` のような「緊張→解放」の複合 affect が既に語彙にあるのは、この用途に非常に都合が良い)。
- ビート列 → 20 次元の**目標曲線** target(t) は `buildTargetVector` の自然な拡張で作れる。
- フォーマット定義自体の実現可能性は**高い**。問題は次の成分 C。

### 5.3 成分 C: アラインメント — 動画のどの時刻がどのビートか (最難関)

これが (3) 全体の律速。正直な評価:

| ゲームの種類 | 手段 | 実現可能性 |
|--------------|------|-----------|
| **自社計装ゲーム (Ludellus 等)** | `play_events` と録画の時刻同期 (録画開始をイベントとして 1 本打つだけ)。`ludellus-tuning-log-design.md` の単調クロック設計がそのまま効く | **高 — CV 不要で正確**。これが本命 |
| 市販ゲーム + 構造化補助情報あり (チャプター、RTA タイマー、コメント内の進行言及) | コメント密度の山谷 + チャプター + LLM によるコメント内容からの進行推定で**粗い**区間割り | 中 — 精度は荒いがビート単位 (分オーダー) なら実用域 |
| 任意の市販ゲーム映像の自動認識 (VLM でキーフレーム分類、UI OCR、シーン検出) | フレームサンプリング → VLM 分類 | **低〜中 — 研究開発案件**。動くデモは作れるが、ゲームごとのプロンプト/ルール調整が要り、コストも 1 動画あたり数百フレーム × VLM で嵩む。汎用化を約束しないこと |

**推奨: 「任意のゲーム動画で全自動」を要件から外す。** (i) 計装ゲームはログ同期で厳密に、(ii) 非計装ゲームはコメント駆動の粗い半自動 + 人手修正 UI、の 2 トラックに割り切る。人手修正を前提にすれば Discutere のペーパーレビューゲート (人間が確認→調整→承認) と同じ UX パターンが流用できる。

### 5.4 成分 D: 「面白さのベクトル」の定量化

- 観測曲線 observed(t) (成分 A) と目標曲線 target(t) (成分 B) が同じ 20 次元空間に乗るので、**時系列版 DesignGap = target(t) − observed(t)** が Discutere の `computeDesignGap` の一次元拡張として素直に定義できる。「意図した緊張が出ていない区間」「意図しない不満が出た区間」が区間単位で定量化され、これはそのまま Discutere の議論フロー (design gap 起点の議論) の入力にもなる。**設計として筋が良い**。
- ただし「面白さ」を単一スカラー曲線に潰すのは推奨しない。面白さの表出は**遅延する** (ボス戦中は fear/anger が立ち、撃破直後に joy が爆発する — この解放こそ `tension_release` の意図通り)。瞬時値の joy を面白さと読むと、良い緊張設計を「つまらない区間」と誤判定する。**ビート単位で「意図 affect との一致度」を面白さの代理指標にする** (瞬時値ではなく区間集約 + 直後区間への持ち越しを見る) のが妥当。
- 統計的注意: 単一動画の曲線はノイズ。同一ゲームの複数動画で正規化時間軸 (ビート基準) に揃えて集約して初めて「ゲームの感情曲線」を名乗れる。A-1 (弾幕) は 1 動画内で既に多視聴者集約になっている点でもここが有利。

### 5.5 (3) の総合判定と現実的なフェーズ分割

**判定: 「動画そのものを AI が観て感情を読む」システムとしては実現可能性が低いが、要求の本質 (ゲーム展開×時間軸で反応と意図の差を定量化する) は既存資産の組み合わせで実現可能性が高い。**

| Phase | 内容 | 新規リスク |
|-------|------|-----------|
| **P1** | sentiment-core 切り出し + niconico 弾幕/YT ライブチャットの時刻付き取り込み → 時間ビン集約で観測感情曲線 (Voluptas 新テーブル `affect_timelines`) | ほぼ無し (既存コードの再配線) |
| **P2** | `game_beat_script` フォーマット定義 + 目標曲線生成 + 時系列 DesignGap。非計装ゲームはコメント駆動の粗アラインメント + 人手修正 UI | 低 |
| **P3** | 計装ゲーム (Ludellus) のログ同期アラインメント — tuning-log 設計の実装と抱き合わせ | 低〜中 (tuning-log 実装自体が前提) |
| **P4 (任意・研究枠)** | 実況音声の ASR → 20 次元 (A-3)、VLM によるビート自動認識の実験 | 高 — 成果を約束しない実験枠として隔離 |

A-2 (表情ベース感情推定) は**どのフェーズにも入れない**ことを明示的に推奨する。

---

## 6. 横断的な注意事項

1. **感情空間の単一正本則を守る**: Discutere は「固定 20 次元、新空間は作らない」を明文化している。Voluptas 側で独自の感情次元を生やした瞬間、Translation Bridge (key 完全一致) と DesignGap の互換が壊れる。20 次元に不足があれば Discutere 側の `VECTOR_SPEC` 改訂として両者同時にバージョンを上げる運用にする。
2. **辞書の口語カバレッジ**: 弾幕コメント (「草」「888888」「!?」) は `lexicon.json` の守備範囲を外れる可能性が高い。P1 で弾幕特化の語彙を lexicon に足すか、cascade の Tier1 (ローカル LLM) を効かせる。ここは実データで早期に検証すべき最初の未知数。
3. **YouTube API quota**: ライブチャットリプレイの取得はコメント取得よりコストが重い。Discutere の `youtube-quota.ts` の管理下に置く。
4. **Voluptas の play_events 保持 90 日**: 感情曲線・ビートアラインメントの原データが 90 日で消えると再計算可能性 (tuning-log 設計の原則 4) が壊れる。`affect_timelines` は派生テーブルとして長期保持し、生イベントのアーカイブ方針と整合させる。
5. **ドキュメントの誠実性**: 設計書に書いてあるが実装されていないもの (PII 暗号化、非同期分析ジョブ、レートリミット) は「未実装」と設計書側に明記する。本レビューの §3.3/§4.3 の指摘は Voluptas リポジトリの issue に転記して追跡することを推奨。

---

## 7. 参照

- Voluptas: `player-profile-backend-design.md` (v1.0 2026-03-16)、`ludellus-tuning-log-design.md` (v1.0 2026-07-02)、`src/services/analysisEngine.js`、`src/routes/auth.js`、`src/services/tokenService.js`
- Discutere: `src/crawler/sentiment/{analyze.mjs,cascade.ts}`、`src/flow/{sentiment-vector.ts,design-gap.ts,persona-adopt.ts,persona-questionnaire.ts,games-md.ts,mechanic-extract.ts}`、`data/affects/vocabulary.json`、`data/games/*.sentiment.json`、`src/crawler/sources/{niconico.ts,youtube-livechat.ts}`
- Cernere: `packages/service-adapter/README.md` (User admission / PeerAdapter)、`packages/id-service/src/id-service.ts`、README「セキュリティ思想」
