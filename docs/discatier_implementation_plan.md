# Discatier 実装計画書

## 0. 文書の位置づけ

本書は Discatier(遊びの議論プラットフォーム)の実装に必要な設計判断・データ構造・入力仕様・実装順序を定義する。AI によるコード生成の入力として使うことを前提に、各セクションは独立した粒度で参照可能になっている。

範囲は **データ保存層とその入出力契約** に限定する。チャット送受信のトランスポート、認証、配信、UI レンダリング技術は本書の範囲外。

設計の核は、3軸の対話モード(学習対話・感情会話・統合)が単一のデータ基盤(Kuzu グラフ + イベントログ)の上で弁証法的ループを形成すること。3軸はそれぞれ異なる参加者・異なる言語規約・異なる権限を持ち、軸の間はブリッジ機構を介してデータが循環する。

---

## 1. 設計原則

### 1.1 弁証法ループ

- **Axis 1 (学習対話)**: 理論家が語彙とメカニクスの定義を精緻化する。意図された美的体験(`intended_affect`)を記録する
- **Axis 2 (感情会話)**: プレイヤーが理論用語を介さず生の感情を報告する。観測された美的体験(`expressed_affect`)が蓄積される
- **Axis 3 (統合)**: 開発者が Axis 1 と Axis 2 のズレ(`DesignGap`)に対して跳躍的な仮説(`Hypothesis`)を提示し、議論を経て検証する

サイクル:
```
Axis 1 (語彙提供) → Axis 2 (ズレ露呈) → Axis 3 (止揚) → 検証済み仮説が Axis 1 に統合 → 継続
```

### 1.2 不変原則

これらは入力 UI ではなくデータ層で強制する。

- **発話の不可改変性**: `Utterance.raw_content` は作成後変更不可。誤解釈で原文が上書きされない
- **権限の分離**: 各軸の参加者は他軸のデータを参照できるが、書き込み権限は自軸に閉じる
- **語彙化されていない感情の保護**: 既存 `Affect` にマップされない発話パターンを定期的に検出し、Axis 1 の議論対象として明示提示する
- **検証なき統合の禁止**: `Hypothesis.integrated` 状態には Axis 2 経由の感情的検証が必須

---

## 2. データモデル

### 2.1 ノード型

| ノード型 | 役割 | 主要プロパティ |
|---------|-----|------------|
| `Person` | 参加者 | id, display_name, roles, created_at |
| `Game` | ゲーム | id, title, platform, release_date, metadata |
| `Mechanic` | メカニクス | id, name, schema_version, current_definition_event_id |
| `Aesthetic` | 美的体験(枠組み別) | id, name, framework_ref |
| `Affect` | 感情語彙 | id, name, parent_id, source, vocabulary_status |
| `PlayContext` | プレイ文脈 | id, game_id, description, conditions |
| `Session` | 対話セッション | id, mode, started_at, ended_at |
| `SynthesisSession` | 統合セッション(`Session` 派生) | + derives_from_session_ids, focus_gap_ids |
| `Utterance` | 発話 | id, session_id, person_id, raw_content, timestamp |
| `Reaction` | 反応 | id, target_utterance_id, person_id, type |
| `DesignGap` | 設計の不備 | id, mechanic_id, intended_affect_id, state, detected_at |
| `Hypothesis` | 提案仮説 | id, content, state, proposed_in_session_id |
| `Event` | イベント | id, event_type, payload, actor_id, timestamp, caused_by_event_id |

補足:

- `Session.mode ∈ {learning, emotion, synthesis}` で軸を区別する
- `SynthesisSession` は `Session` の派生型。Kuzu 上は `Session` テーブルにモードを持たせ、追加プロパティを別テーブルで保持する形でモデル化する
- `Person.roles ⊆ {theorist, player, developer}` で複数役を許容
- `Mechanic.current_definition_event_id` でイベントログを指す。実定義はイベントから projection で構築する(Event Sourcing)
- `Affect.vocabulary_status ∈ {established, proposed, deprecated}` で語彙の成熟度を管理

### 2.2 エッジ型

参照系:
- `Utterance -[:authored_by]-> Person`
- `Utterance -[:in_session]-> Session`
- `Utterance -[:refers_to]-> Mechanic | Aesthetic | Game | Hypothesis`
- `Utterance -[:expresses]-> Affect`
- `Person -[:participated_in]-> Session`

応答系:
- `Utterance -[:responds_to]-> Utterance` (応答)
- `Utterance -[:resonates_with]-> Utterance` (共鳴: 別構造で扱う)
- `Utterance -[:clashes_with]-> Utterance` (温度差)
- `Reaction -[:reacts_to]-> Utterance`

意味系(素材層内):
- `Mechanic -[:intends]-> Affect`
- `Mechanic -[:composes]-> Mechanic`
- `Mechanic -[:variant_of]-> Mechanic`
- `Affect -[:parent]-> Affect`

問題系:
- `DesignGap -[:gap_in]-> Mechanic`
- `DesignGap -[:expected_affect]-> Affect`
- `DesignGap -[:observed_affect]-> Affect`
- `DesignGap -[:evidence]-> Utterance`

仮説系:
- `Hypothesis -[:proposed_in]-> SynthesisSession`
- `Hypothesis -[:addresses]-> DesignGap` (**必須**)
- `Hypothesis -[:validated_by]-> Session` (プロパティ: validation_role ∈ {theory, emotion})
- `Hypothesis -[:integrates_as]-> Mechanic`

派生系:
- `SynthesisSession -[:derives_from]-> Session` (**最低1つ必須**)

定義更新系:
- `Utterance -[:refines]-> Mechanic` (この発話が定義更新を引き起こした)
- `Utterance -[:refines]-> Affect`

### 2.3 SynthesisSession の特性

`Session` の派生型として実装し、以下の追加制約を持つ。

- `derives_from` エッジを必ず1つ以上持つ(空からは始まらない)
- `focus_gap_ids` に少なくとも1つの `DesignGap` を指定して起動する
- セッション内で生まれる `Hypothesis` は自動的にこのセッションを `proposed_in` として持つ

これにより以下のクエリが自然に書ける:

```cypher
// この仮説の根拠を全て辿る
MATCH (h:Hypothesis)-[:proposed_in]->(ss:SynthesisSession)
      -[:derives_from]->(s:Session)<-[:in_session]-(u:Utterance)
WHERE h.id = $hypothesis_id
RETURN u, s
```

### 2.4 Event Sourcing

すべてのグラフ変更は append-only イベントとして記録する。

イベント種別:
- `MechanicProposed`, `MechanicRefined`, `MechanicDeprecated`
- `AffectAdded`, `AffectRefined`
- `UtteranceCreated`
- `ReactionAdded`
- `GapDetected`, `GapResolved`, `GapDismissed`
- `HypothesisProposed`, `HypothesisDebated`, `HypothesisValidated`, `HypothesisIntegrated`, `HypothesisRejected`
- `SessionStarted`, `SessionEnded`

各イベントは以下の構造:

```typescript
{
  event_id: UUID,
  event_type: EventType,
  payload: Record<string, unknown>,
  actor_person_id: UUID,
  timestamp: ISO8601,
  caused_by_event_id: UUID | null  // 因果連鎖
}
```

`Mechanic` 等の状態ノードは projection によって最新状態が再構築される。**グラフは "現在のビュー"、イベントログは "真実のソース"**。

projection は起動時にメモリ上で構築し、各イベント受信時に増分更新する。グラフへの永続化はキャッシュとして扱い、整合性が壊れた場合はイベントから再構築可能。

---

## 3. 入力仕様(チャットベース)

3軸はすべてチャットスレッドのモードとして実装される。共通の入力インタフェースはメッセージ送信のみ。モード切替は Session 起動時のメタ情報で行い、同一スレッド内でモードが混在することはない。

- Session = 1つのチャットスレッド
- Utterance = スレッド内の1メッセージ
- 1スレッドに複数人が参加可能
- メッセージは作成後不可改変(訂正は別メッセージで行い、`responds_to` で関連付ける)

### 3.1 Axis 1: 学習対話チャット

**参加者**: 理論家(複数可)、システム(語彙提案役)
**開始時の指定**: 対象 Mechanic または対象 Game

メッセージは自由記述、マークダウン対応。発話は理論用語を含んでよい。

システム応答パターン:
- 発話に対する `refers_to` 候補の提示(明示的な確認を要求)
- 関連する既存 Mechanic / Affect の引用
- 未語彙化クラスタの通知(週次または重要発見時)
- スラッシュコマンドの実行結果

スラッシュコマンド:
- `/define <mechanic_name>` — 新規 Mechanic の定義開始
- `/intends <affect>` — 現在対象の Mechanic に意図された Affect を追加
- `/refine <mechanic>` — 既存 Mechanic の定義更新(直前または指定発話を `refines` で紐付け)
- `/compare <mechanic_a> <mechanic_b>` — 比較ビューを構造化テキストで返却
- `/history <mechanic>` — 定義の変遷を時系列で返却

権限境界: Axis 2 / Axis 3 のメッセージは参照可能、編集不可。

### 3.2 Axis 2: 感情会話チャット

**参加者**: プレイヤー(複数可)、システム(聞き役)
**開始時の指定**: 対象 Game(必須)、場面(任意)

メッセージは自由記述、絵文字リアクション可。「なんかダルかった」「ここで震えた」レベルの素朴な日本語が前提。

システム応答方針:
- 基本は受身。能動的に語らない
- 沈黙が長ければ「他にはありますか?」程度の最小限の促し
- 理論用語を絶対に出力しない(MDA、Aesthetic、専門 Affect 名等)
- スレッド冒頭に対象 Game のタイトルだけ表示し、それ以外のメタ情報は隠す

スラッシュコマンド(最小限):
- `/scene <description>` — 場面コンテキストを更新
- `/done` — セッション終了

裏側処理:
- 各メッセージに対し LLM が `proposed_expresses(Affect)` と `proposed_refers_to(Mechanic)` を仮付加
- 仮エッジは Axis 1 のレビュー対象
- プレイヤーには裏側処理の存在を提示しない

設計鉄則: **ボットの応答は最小限**。プレイヤーが理論的な構造化を求めない以上、ボットも構造化された返答をしない。

### 3.3 Axis 3: 統合チャット (SynthesisSession)

**参加者**: 開発者(複数可)、システム(議論補助役)
**開始時の指定**: `derives_from` Session を1つ以上、対象 Gap を1つ以上(両方必須)

メッセージは自由記述。Hypothesis のドラフトはコマンド経由。

システム応答パターン:
- スレッド冒頭に対象 Gap の要約と参照 Session の概要を固定表示(ピン留めメッセージ相当)
- `/propose` 検出時に Hypothesis ドラフトを構造化
- Hypothesis 状態変化を通知
- 検証経路の状態をオンデマンド表示

スラッシュコマンド:
- `/refs <session_ids>` — 参照 Session を追加
- `/propose <title>` — Hypothesis ドラフト開始
- `/addresses <gap_id>` — 現在のドラフトが対象とする Gap を明示(**提案確定の必須条件**)
- `/jump` — 「Axis 1 にない概念」のセルフ宣言
- `/submit` — ドラフトを `proposed` 状態で確定(`/addresses` 実行済みなら成功)
- `/validate theory` — 理論検証 Session への送付
- `/validate emotion` — プレイヤー検証 Session への送付
- `/integrate` — `validated_by_emotion` 状態でのみ受付

強制制約(コマンドハンドラレベル):
- `/submit` は `/addresses` が事前に実行されていない場合エラー
- `/integrate` は仮説状態が `validated_by_emotion` でない場合エラー

### 3.4 横断コマンド

すべての軸から利用可能:
- `/me role <theorist|player|developer>` — 役割切替(セッション再起動を伴う)
- `/find <query>` — グラフ全文 + ベクトル検索
- `/lineage <mechanic_id>` — メカニクスの定義変遷とそれを生んだ発話を表示
- `/cluster` — 未語彙化感情クラスタの最新(主に Axis 1 から)
- `/stale` — 30日停滞 Hypothesis 一覧(主に Axis 3 から)
- `/hotspot` — 議論活発度・Gap 集中度の高い Mechanic を表示

### 3.5 メッセージから graph data への射影

メッセージ送信時に次の処理が走る。

1. `UtteranceCreated` イベントを発行
2. 本文から `responds_to` を推論(直前メッセージまたは明示的引用から)
3. `session.mode` に応じた処理:
   - `learning`: スラッシュコマンドを解釈、なければ Mechanic / Affect 関連の候補抽出
   - `emotion`: Translation Bridge への入力
   - `synthesis`: スラッシュコマンドを解釈、Hypothesis 進行管理
4. 絵文字リアクションは `Reaction` ノードとして生成
5. `resonates_with` / `clashes_with` は明示的引用 + 短い反応メッセージから候補提示のみ(完全自動化はしない)

メッセージ本文は不可改変。誤りの訂正は新規メッセージで `responds_to` 関係を作って表現する。

---

## 4. ブリッジ機構

### 4.1 Translation Bridge (Axis 2 → Axis 1)

Axis 2 の発話を Axis 1 の語彙へ翻訳する半自動パイプライン。

**トリガー**: `UtteranceCreated` イベント(`session.mode = emotion` の場合)

**処理フロー**:
1. 発話を埋め込みベクトル化
2. 既存 `Affect` との類似度を計算(コサイン類似度 + LLM 判定)
3. 既存 `Mechanic` の `intends → Affect` と PlayContext から関連 Mechanic 候補を推論
4. `Utterance` に `proposed_expresses` と `proposed_refers_to` を仮エッジとして付加
5. Axis 1 の「未確定マッピング」キューに追加

**レビュー結果**:
- 承認: 仮エッジを正規エッジに昇格
- 修正: 別の Affect / Mechanic にマッピングし直す
- 棄却: 仮エッジ削除、`uncategorized` フラグ付与
- 「新語彙が必要」: `Affect` 候補を提案、未語彙化クラスタへ集約

レビューの過程自体が Axis 1 セッション内で `Utterance` を生み、`refines → Affect` エッジで Affect 定義の更新につながる。Axis 2 の発話が Axis 1 の語彙を成長させる経路が明示される。

### 4.2 Gap Detection (Axis 1 ∩ Axis 2 → DesignGap)

Axis 1 の意図と Axis 2 の観測のズレを定期的に検出する。

**実行頻度**: 1日1回バッチ、または Axis 1/2 セッション終了時のフック

**処理フロー**:
1. すべての `Mechanic` に対して、`intends → Affect` を取得(期待)
2. その Mechanic が参照されている Axis 2 の `Utterance` から `expresses → Affect` を集約(観測)
3. 以下の条件で Gap 候補:
   - 期待 Affect が観測に含まれない
   - 観測に強い負の Affect が含まれる(`Affect.valence` 等で判定)
   - 観測サンプル数が最低3以上(ノイズ閾値)
4. 既存 `DesignGap` と重複チェック
5. 新規 Gap を作成 (`GapDetected`)

**手動 Gap 作成**: Axis 3 の開発者は手動で Gap を作成できる。ただし `evidence` エッジで Axis 2 発話への根拠提示が必須。

### 4.3 Hypothesis Lifecycle (Axis 3 → 検証 → Axis 1)

`Hypothesis` の状態機械:

```
proposed → debated → validated_by_theory → validated_by_emotion → integrated
                              ↘                                  ↗
                                rejected (どの段階からでも遷移可)
```

**遷移ルール**:

| 遷移 | 条件 | トリガー |
|------|------|----------|
| proposed → debated | SynthesisSession で1回以上の `Utterance` が `refers_to` した | 自動 |
| debated → validated_by_theory | Axis 1 セッションで承認発話を受領 | 半自動(明示承認) |
| validated_by_theory → validated_by_emotion | Axis 2 セッションでプレイヤーの確認発話 | 半自動(試遊フィードバック) |
| validated_by_emotion → integrated | `/integrate` 受領 → `MechanicProposed` / `MechanicRefined` を発行 | 手動 |
| 任意 → rejected | 明示的な却下 | 手動 |

**孤立検出**: 状態が30日変化していない `Hypothesis` を週次でレポート。`/stale` コマンドで取得可能。`debated` 状態の停滞を最優先で表示。

---

## 5. データ保存スタック

データ保存に関わる要素のみを定義する。アプリケーション層・通信層・入出力チャネルの実装技術は本書の範囲外。

| 役割 | 技術 | 備考 |
|------|------|------|
| グラフ + ベクトル + イベント保存 | Kuzu | 単一ファイル、組込み運用、Cypher 系クエリ言語 |
| 埋め込み生成 | sentence-transformers (multilingual) または OpenAI text-embedding-3-small | 日本語対応必須。Kuzu の ARRAY カラムに格納 |
| バックアップ | Kuzu DB ファイル + イベントログ JSONL ダンプ | イベントが真実のソースなので JSONL のみで完全復旧可能 |

データ形式の決定事項:
- ノード・エッジは Kuzu スキーマで型付け(本書 §2)
- ベクトルは Kuzu の ARRAY 型。次元は採用する埋め込みモデルに合わせて固定
- イベントは Kuzu の `Event` テーブルに格納。`payload` は JSON 文字列としてテキストカラムに直列化
- イベントは並行して JSONL ファイルにも書き出す(append-only、ローテーション可)
- 将来の DB 移行(例: Postgres + Apache AGE)が必要になった場合は JSONL から再構築する

スコープ外:
- チャット送受信のトランスポート(WebSocket、HTTP、メッセージング基盤等)
- 認証・セッション管理
- LLM 呼び出しクライアント実装
- フロントエンド・UI レンダリング

これらは保存層の上に乗るアプリケーション側の判断とし、本書では **保存層への入出力契約** のみを定める。アプリ層への契約は §2 のノード/エッジ定義 + §3 のメッセージ射影規約 + §4 のブリッジ処理 で表現される。

---

## 6. 実装フェーズ

各フェーズは独立して完了判定可能。AI 生成と人手作業の役割分担を明示する。

### Phase 0: 基盤設計の固定(人手)

AI 生成前にこれだけは手で確定する。

作業項目:
- Kuzu スキーマ DDL の確定(本書 §2 に基づく)
- イベント型と payload 構造の TypeScript / Rust 型定義
- `Affect` 初期語彙(20〜40件)の手動投入(日本語の感情語彙中心、§9 参照)
- 埋め込みモデルの選定とベクトル次元の確定

完了条件:
- スキーマ migration スクリプトが実行可能
- 初期 Affect が DB に入り、Cypher で取得できる
- Event 型のテスト用ペイロードが round-trip する

### Phase 1: 保存層コア API(AI 生成)

作業項目:
- ノード・エッジの CRUD
- イベントログ書き込み + projection の整合
- Utterance / Reaction / Session の基本フロー
- ベクトル登録・検索 API

完了条件:
- 1つの Session に 1つの Utterance を記録し、Event 経由で復元可能
- 全 API が型付きで単体テストを持つ

### Phase 2: メッセージ射影層(AI 生成、コマンドパーサは人手で精査)

作業項目:
- メッセージ受信 → `UtteranceCreated` への変換
- スラッシュコマンドの解釈と検証
- `responds_to` / `Reaction` の自動生成
- モード別ハンドラの分岐

完了条件:
- 3軸それぞれの主要コマンドが正しく解釈される
- 不正コマンド(`/integrate` の前提未達など)が拒否される

### Phase 3: Translation Bridge(AI 生成、プロンプトは人手調整)

作業項目:
- 埋め込み生成パイプライン
- 類似 Affect 検索
- LLM 推論ラッパー(プロンプト含む)
- 仮エッジの付加と pending_review フラグ
- レビュー結果の正規エッジ化

完了条件:
- 感情モードの Utterance が自動で `proposed_expresses` を持つ
- レビュー → 正規エッジ化される E2E が通る

### Phase 4: Gap Detection(AI 生成)

作業項目:
- バッチジョブ実装
- Mechanic ↔ Utterance のマッチング
- Gap 候補生成と既存 Gap との重複チェック

完了条件:
- シードデータで Gap 検出が走り、少なくとも1件の Gap が自動生成

### Phase 5: Hypothesis Lifecycle と検証パス(AI 生成、状態遷移は人手で精査)

作業項目:
- 状態機械の実装
- 検証パスの起動処理(検証先 Session の Hypothesis 参照付与)
- 孤立検出ジョブ

完了条件:
- 1つの Hypothesis が `proposed` から `integrated` まで遷移する E2E が通る
- 30日停滞検出が走る

### Phase 6: 横断クエリコマンド(AI 生成)

作業項目:
- `/lineage`, `/cluster`, `/stale`, `/hotspot`, `/find` の実装
- 未語彙化クラスタリングのバッチジョブ
- ホットスポット集計のバッチジョブ

完了条件:
- 各コマンドが構造化テキストで結果を返す

---

## 7. リスクと対策

### 7.1 軸間の権力勾配

リスク: Axis 1 が Axis 2 の発話を矮小化、Axis 3 が独走、Axis 1 の語彙が固定化

対策:
- `Utterance.raw_content` の不可改変性(DB 制約)
- `Hypothesis.integrated` への遷移はコマンドレベルで Axis 2 検証を要求(`/integrate` ハンドラで状態検証)
- 未語彙化クラスタの常時可視化(`/cluster` で取得)
- イベントログによる「誰がいつ何を上書きしたか」の完全追跡

### 7.2 仮説の孤立

リスク: Axis 3 が空想ゲーム化し、`debated` 状態の Hypothesis が大量蓄積

対策:
- `Hypothesis.addresses → DesignGap` を必須にし、根拠なき提案を構造的に禁止
- 状態が30日停滞している Hypothesis の週次レポート(`/stale`)

### 7.3 Translation Bridge の誤訳

リスク: LLM がプレイヤーの感情を誤解釈し、誤ったマッピングが正規化される

対策:
- 自動承認しない(常に Axis 1 の人手レビュー経由)
- 原文 `Utterance.raw_content` は不変なので、誤マッピングは後から修正可能
- 修正経緯自体が `Affect` 定義の議論材料になる(`refines` エッジで追跡)

### 7.4 語彙の早期固定化

リスク: Axis 1 が初期語彙を作った後、新規 Affect の追加が滞る

対策:
- 未語彙化クラスタの可視化(§3.4 の `/cluster`)
- 「新語彙が必要」レビュー結果の積極利用
- 月次で「最近追加された Affect」「最近修正された Mechanic」の活動レポート

### 7.5 Kuzu の機能限界

リスク: Kuzu はまだ若く、特定機能(複雑な集約、マルチユーザー同期等)が不足する可能性

対策:
- マルチユーザー同期は当初想定外。後で必要になれば Postgres + Apache AGE への移行パスを保留
- スキーマは Kuzu 固有機能に深く依存しない範囲で書く
- イベントログを真実のソースにすることで、DB が変わってもデータは保全される

---

## 8. 受け入れ基準

実装完了の判定基準(統合テスト相当):

1. 3軸それぞれで実データを蓄積できる(Axis 1 でメッセージ + コマンドから Mechanic 定義、Axis 2 で感情発話、Axis 3 で Hypothesis 提案)
2. 1つの Mechanic について、Axis 1 の `intended_affect` と Axis 2 の `expressed_affect` のズレが Gap として自動検出される
3. Gap に対する Hypothesis が SynthesisSession で提案され、状態機械が回って `integrated` まで到達する E2E シナリオが通る
4. `/lineage` コマンドで、任意の Mechanic の現在定義からそれを生み出した発話・議論・検証を3軸横断で辿れる
5. `/cluster` コマンドで未語彙化感情クラスタが取得できる
6. `/stale` コマンドで30日停滞 Hypothesis が取得できる
7. `Utterance.raw_content` の書き換え試行が DB レベルで拒否される

---

## 9. 補遺: 初期 Affect 語彙の暫定リスト

Phase 0 で投入する語彙の候補。日本語の自然な感情語を中心に、暫定的に Lazzaro / MDA との対応を記す。

| Affect | 親 | Lazzaro 対応 | MDA Aesthetics 対応 |
|--------|-----|-------------|---------------------|
| ワクワク | positive | Hard Fun | Challenge / Discovery |
| 達成感 | positive | Hard Fun | Challenge |
| 没入 | positive | Serious Fun | Submission |
| 発見 | positive | Easy Fun | Discovery |
| 共感 | positive | People Fun | Fellowship |
| ゾクッ | positive | Serious Fun | Sensation |
| ホッ | positive | Serious Fun | Sensation |
| スッキリ | positive | Hard Fun | Challenge |
| イライラ | negative | — | — |
| ダルい | negative | — | — |
| 困惑 | neutral/negative | — | — |
| 拍子抜け | negative | — | — |
| 理不尽 | negative | — | — |
| もどかしい | negative | — | — |
| 飽きた | negative | — | — |

これらは出発点であり、運用開始後の未語彙化クラスタ検出によって随時拡張される前提。
