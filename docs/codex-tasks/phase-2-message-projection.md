# Phase 2: メッセージ射影層

設計参照: `docs/discatier_implementation_plan.md` §3 (入力仕様) / §3.5 (メッセージから graph data への射影)

## ゴール

チャットメッセージを `UtteranceCreated` イベントに変換し、 `session.mode` に応じてスラッシュコマンドを解釈する。 不正コマンド (前提未達等) を DB に到達する前に拒否する。

## 前提

- Phase 1 完了 (merge hash 確認)。 `appendEvent` / リポジトリ API が使える。

## ディレクトリ構成 (新規)

```
src/core/
  projection/
    message-input.ts         # メッセージ入力エントリ関数
    command-parser.ts        # スラッシュコマンドのトークン化
    command-handlers/
      learning/              # Axis 1: /define /intends /refine /compare /history
        define.ts
        intends.ts
        refine.ts
        compare.ts
        history.ts
        index.ts
      emotion/               # Axis 2: /scene /done
        scene.ts
        done.ts
        index.ts
      synthesis/             # Axis 3: /refs /propose /addresses /jump /submit /validate /integrate
        refs.ts
        propose.ts
        addresses.ts
        jump.ts
        submit.ts
        validate.ts
        integrate.ts
        index.ts
      cross/                 # 横断: /me /find /lineage /cluster /stale /hotspot
                              # Phase 6 で本実装。 ここでは stub のみ
        index.ts
    responds-to-inference.ts # 直前メッセージや明示引用から推論
    reaction-handler.ts      # 絵文字 → Reaction
tests/core/projection/
  command-parser.test.ts
  learning-handlers.test.ts
  emotion-handlers.test.ts
  synthesis-handlers.test.ts
  e2e-axis1-flow.test.ts
  e2e-axis3-submit-guard.test.ts
```

## サブタスク

1. **コマンドパーサ**
   - `parseCommand(content): { command: string; args: string[] } | null` を実装。
   - 行頭 `/` + 第1トークン = command、 以降はスペース区切り (引用符対応)。

2. **メッセージ入力エントリ**
   - `submitMessage({ sessionId, personId, raw_content }): UtteranceId` を提供。
   - 処理順:
     1. `responds_to` 推論 (直前メッセージ or 明示引用)
     2. `session.mode` に応じてコマンドハンドラ分岐
     3. コマンドでなければ通常 Utterance として projection
     4. `UtteranceCreated` event 発行 + Utterance ノード化

3. **Axis 1 ハンドラ (`learning/`)**
   - `/define <name>`: 新規 Mechanic 作成 → `MechanicProposed` event。
   - `/intends <affect>`: 対象 Mechanic に `intends → Affect` 付加 → `MechanicRefined`。
   - `/refine <mechanic>`: 直前/指定発話を `refines` 紐付け → `MechanicRefined`。
   - `/compare <a> <b>`: 構造化テキスト返却 (副作用なし)。
   - `/history <mechanic>`: 定義変遷を時系列で返却 (副作用なし)。

4. **Axis 2 ハンドラ (`emotion/`)**
   - `/scene <description>`: Session メタの場面コンテキスト更新。
   - `/done`: Session 終了 → `SessionEnded`。
   - その他のメッセージは plain Utterance + (Phase 3 で Translation Bridge に渡す hook 点を残す)。

5. **Axis 3 ハンドラ (`synthesis/`)**
   - `/refs <session_ids>`: `derives_from` 追加 (Session 起動時のみ受付)。
   - `/propose <title>`: Hypothesis ドラフト開始 (`proposed_in` を current session に)。
   - `/addresses <gap_id>`: 現在ドラフトの `addresses → DesignGap` 設定。
   - `/jump`: 「Axis 1 にない概念」のセルフ宣言フラグ。
   - `/submit`: ドラフト確定。 **`/addresses` 未実行ならエラー**。 成功時 `HypothesisProposed` event。
   - `/validate theory|emotion`: 検証 Session への送付。
   - `/integrate`: 状態 `validated_by_emotion` でないとエラー → 成功時 `HypothesisIntegrated`。

6. **横断コマンド stub**
   - `cross/index.ts` で `/me /find /lineage /cluster /stale /hotspot` を受けて「Phase 6 で実装」 stub 返却。

7. **Reaction ハンドラ**
   - 絵文字リアクション payload を受けて `Reaction` ノード + `ReactionAdded` event。

8. **`responds_to` 推論**
   - 直前メッセージ自動紐付け + `> @uid:xxx` のような明示引用記法対応。

## 受け入れ条件

- [ ] 3軸それぞれの主要コマンドがテストで正しく解釈される (各軸 1 ケース以上)。
- [ ] **`/submit` の前に `/addresses` が無いと拒否される** (E2E テスト必須)。
- [ ] **`/integrate` は `validated_by_emotion` 状態でないと拒否される** (E2E テスト必須)。
- [ ] 通常メッセージは `UtteranceCreated` event を発行し、 `Utterance` ノードと `responds_to` エッジが正しく作られる。
- [ ] 絵文字リアクションが `Reaction` + `ReactionAdded` 化する。

## スコープ外

- LLM 呼び出し / Translation Bridge (Phase 3)
- Gap 自動検出 (Phase 4)
- 横断コマンドの本実装 (Phase 6)

## コミット & PR

- ブランチ: `feat/discatier-phase-2-message-projection`
- base: Phase 1 の merge hash
- PR 単位: 1 PR で完結。 ハンドラが多いので最大 2 分割可 (Axis 1+2 / Axis 3) 。
