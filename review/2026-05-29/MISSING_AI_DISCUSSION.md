# MISSING_AI_DISCUSSION — Discutere (2026-05-29)

「議論の自走 (AI 単独サイクル)」 と 「人間 ↔ AI 対話」 の観点で、 現状の Phase 0
スタック (Discatier Core + crawler + visualize + persona-engine) に **不足している実装** の
洗い出し。 REVIEW_MISSING_FEATURES.md の続編 / 議論レイヤ特化版。

ベースライン: main = `05ed7fd` (PR #20 / #21 / #22 マージ後)。

## ① 議論の自走 (AI 単独サイクル)

| # | 不足 | 現状 | 優先 | 規模 |
|---|------|------|------|------|
| 1-1 | rule engine ↔ Discatier event 配線 | persona-engine `fireEvent({kind: "GapDetected"})` を呼ぶ呼出元が無い。 Discatier が gap 検出した瞬間に自動で叩く glue が要る | High | S |
| 1-2 | 議論セッション (Session) の自動継続 | persona が utterance 出す時の sessionId は手動指定。 「ある gap について議論を継続する」 持続的 session の自動生成が無い | High | M |
| 1-3 | persona の active 化判定 | 全 persona が常に発火対象。 「ある議論には advocate + sceptic + integrator だけ呼ぶ」 セッション別 persona pool が無い | High | M |
| 1-4 | 議論の終結条件 | hypothesis が validated / rejected / integrated になっても rule engine は止まらない。 「議論クローズ」 events 未定義 | High | S |
| 1-5 | **再帰 / 無限ループ防止** | propose_hypothesis → イベント連鎖で再度 propose-on-gap 発火可能。 グローバル発火回数上限 (per session / per gap) なし | **Critical** | S |
| 1-6 | 議論深度の制御 (turn budget) | 1 議論あたりの utterance / hypothesis 数上限なし。 LLM コスト青天井 | High | S |
| 1-7 | 静かな時間帯 (quiet hours) | Concordia は深夜帯 1/10 throttle 実装あり、 persona-engine は未移植 | Low | S |

## ② 人間 ↔ AI 対話 (入口 / 出口)

| # | 不足 | 現状 | 優先 | 規模 |
|---|------|------|------|------|
| 2-1 | Discord webhook 署名検証 | `routes.ts:780-798` で Ed25519 未検証 = 第三者投入可 | **Critical** (REVIEW 2026-05-29 既出) | S |
| 2-2 | Slack webhook 署名検証 | 同上、 X-Slack-Signature HMAC ゼロ | **Critical** | S |
| 2-3 | Discord Interactions endpoint | `/api/discord/interactions` route 不在、 verifyDiscordInteraction() は main に未着地 | High | M |
| 2-4 | Discord slash command 一式 | `/propose` `/validate` `/refute` `/integrate` 等、 議論用 slash 未実装。 `command-parser.ts` は文字列 parse できるが Discord Interactions 経由の発火経路がない | High | M |
| 2-5 | 人間 → AI へ「介入」 経路 | 「この persona をこの議論で active に」 「この rule を 1 時間止めて」 等の人間操作 API 不在 (Concordia は `setRulesEnabled` を web UI 公開) | High | M |
| 2-6 | AI 提案の人間 review UI / Discord embed | hypothesis 提案 → 人間が approve / reject する path なし。 Discatier の `/validate` command はあるが Discord Interactions 経由で発火させる UI なし | High | M |
| 2-7 | session 永続化 (mode-state.ts) | task / discussion mode session が `Map<>` オンメモリ。 再起動で消失 (REVIEW Di-9) | High | M |
| 2-8 | 人間発言と persona 発言の区別 | `utterances.speaker_id` は string、 persona は `"persona:<id>"` prefix で区別する規約のみ。 query / view で混在しても分離できない | Medium | S |
| 2-9 | 人間 → persona feedback | 「この persona の発言は鋭くて良かった」 を `persona_feedback` に書き戻す API / UI なし。 schema は持っている | Medium | S |

## ③ 学習 / フィードバック

| # | 不足 | 現状 | 優先 | 規模 |
|---|------|------|------|------|
| 3-1 | learned_notes 自動更新 | hypothesis が integrated / rejected された結果を persona に紐付けて learned_notes 蓄積する経路なし (persona-engine DESIGN で Phase 1 と明言) | High | M |
| 3-2 | persona パフォーマンス指標 | 「advocate の提案 採用率 32%」 等の集計 view なし | Medium | M |
| 3-3 | rule の効果測定 | rule_log はあるが、 「この rule が誘発した hypothesis のうち validated 率」 計測なし | Medium | M |
| 3-4 | 議論メモリ (cross-session 知見) | 同じ gap が別 session で再議論された時、 過去議論を参照する経路なし (historian persona の素材無し) | Medium | M |
| 3-5 | vector embedding 接続 | `src/core/vectors/` 既存だが persona-engine prompt-builder で類似 hypothesis を引いていない。 RAG 化が未連携 | Medium | M |

## ④ 観測 / 統治 / 安全

| # | 不足 | 現状 | 優先 | 規模 |
|---|------|------|------|------|
| 4-1 | 議論可視化 dashboard | visualize は md 出力のみ。 「現在何 session 動いてる / どの persona が何回発話 / cooldown 状況」 を見るリアルタイム view なし | High | M |
| 4-2 | runtime kill switch GUI | `engine.setRulesEnabled(false)` は API 化されていない、 Concordia の admin Web UI 相当を Di に持ってきていない (Phase 1) | High | M |
| 4-3 | コスト計測 | LLM 呼び出し回数 / token は `AnthropicSdkClient` から `usage` 返るが集計 store なし | Medium | S |
| 4-4 | 議論 audit log | `rule_log` はあるが「どの human / どの persona がいつ何をしたか」 の統一 audit log なし | Medium | S |
| 4-5 | JWT_SECRET production guard | default value で本番起動可能 (REVIEW 既出) | High | S |
| 4-6 | resolveAssignee 実装 | webhook-handler.ts:247 TODO 放置 → task completion 検出後 assignee 解決されない | High | S |
| 4-7 | session timeout / cleanup | mode-state.ts のセッションは TTL なし、 メモリリーク経路 | Medium | S |
| 4-8 | **phase2 synthesis-handlers.test failure** | `/validate emotion` が ok=false で返る。 議論の検証→統合フローが切れている | High | M |

## ⑤ visualize / persona-engine 自身の足回り

| # | 不足 | 現状 | 優先 | 規模 |
|---|------|------|------|------|
| 5-1 | visualize に persona type | `[[persona:<id>]]` 規約は DESIGN.md にあるが exporter 未実装。 議論 md から persona md へジャンプ不可 | Medium | S |
| 5-2 | persona 発言の自動 md export | utterance 投稿時に `data/discussions/.../utterance/<id>.md` を自動書出 (= 議論ログの永続 PR 文化化) なし | Medium | S |
| 5-3 | prompt-builder の cross-session context | 現状 1 session 内の utterance しか引かない、 過去議論や他 session 知見は無視 | Medium | M |
| 5-4 | `@anthropic-ai/sdk` 直依存 | persona-engine は fetch 直叩きで SDK 依存なしに作ったが、 prompt caching / streaming 使うには SDK か手書き拡張が要る | Low | M |
| 5-5 | MockLLMClient テスト helper の不足 | scripted response のみ、 「prompt 中の特定文字列で分岐」 等の高度モック未対応 | Low | S |

---

## 優先度サマリ (decision-metrics)

### 最初に潰すべき (Critical / High 規模 S-M)

| 順 | 項目 | AI 学習量 | 作業コスト | 達成度 | 主目的一致 |
|----|-----|---|---|---|---|
| 1 | 1-5 無限ループ防止 | 4 | 低 (per-session 発火カウンタ 1 つ) | 5 | 5 |
| 2 | 2-1 / 2-2 webhook 署名検証 | 3 | 中 (両 platform 実装 + テスト) | 5 | 5 |
| 3 | 1-1 rule engine ↔ Discatier event 配線 | 5 | 中 (EventBus 追加 + Discatier hook) | 5 | 5 |
| 4 | 4-8 phase2 `/validate emotion` 修正 | 4 | 中 (synthesis-handlers bug 解析) | 5 | 5 |
| 5 | 1-4 議論終結条件 + 1-6 turn budget | 4 | 低 (rule の event_kind 追加) | 4 | 4 |

### 次に効く (High / Medium 規模 M)

- 2-3 Discord Interactions endpoint (slash command 経由で人間が直接議論操作)
- 2-7 session 永続化 (再起動安全)
- 4-2 runtime kill switch API + 4-1 dashboard (暴走時の止め方)
- 3-1 learned_notes 自動更新 (学習ループ閉じる)
- 1-3 persona pool (session ごとに persona を絞る)

### 後でやれば良い (Low)

- 1-7 quiet hours
- 5-4 SDK 移行
- 3-5 vector embedding 統合

---

## 1 PR 単位の推奨スコープ

それぞれ独立 PR にできるグルーピング案:

| PR | 含める項目 | 規模 |
|----|------------|------|
| **A. safety guard** | 1-5 + 1-6 + 2-1 + 2-2 (無限ループ / turn budget / 両 webhook 署名検証) | M |
| **B. discord interactions** | 2-3 + 2-4 + 2-5 (slash command 一式 + 人間介入 API) | L |
| **C. persistence + cleanup** | 2-7 + 4-7 + 4-5 (session 永続化 + TTL + JWT guard) | M |
| **D. event wiring** | 1-1 + 1-2 + 1-4 (Discatier event → rule engine 配線 + 議論 session + 終結) | M |
| **E. visualize 拡張** | 5-1 + 5-2 (persona md + utterance 自動 export) | S |
| **F. learning loop** | 3-1 + 3-2 + 3-3 (persona feedback + 指標) | M |
| **G. phase2 fix** | 4-8 (synthesis-handlers.test 修正) | S |
| **H. dashboard + admin** | 4-1 + 4-2 (runtime view + kill switch GUI) | L |

---

## 進め方の提案

PR-G (phase2 fix) と PR-A (safety guard) を最優先で潰し、 続いて PR-D (event wiring)
で 「議論が自動で開始 → 終結する」 ループを閉じる。 これで「議論自走」 の最小骨格が完成。

その後、 PR-B (Discord interactions) + PR-C (永続化) + PR-H (dashboard) で
「人間が安全に観察 / 介入できる」 経路を整える。 PR-F (learning loop) は最後、
データが蓄積し始めてからの方が設計しやすい。

PR-E (visualize 拡張) は他 PR とは独立で並列実装可。
