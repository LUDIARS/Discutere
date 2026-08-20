# Anatomia 事前情報ブリッジ — ソースコード解析を議論の事前情報に

## 目的

Di の議論はゲームデザインの「狙った体験」と「観測された体験」を突き合わせる。その材料
(ディスカッションペーパーの事前情報) は従来 `data/games/*.md` のメカニクス + 外部の声
(クロール) だけだった。**実装中のゲーム (LUDIARS リポ等) には game md が無い**ため、その
ゲーム自体を議題にすると事前情報が空に近くなり、ペルソナが憶測で喋る。

このブリッジは [Anatomia](../../Anatomia)(ソースコード解析の建築規約オラクル)が対象リポから
抽出した**ドメイン構造**を取得し、**player-facing なメカニクスに精製**して事前情報に差し込む。
= 「コードがそこにある実装中のゲーム」をそのまま Di の議題にできる。

> 既存の Di↔Anatomia 接続は **cost-relay** (Di→Anatomia へ LLM コストを push) のみ。本ブリッジは
> 逆向き (Di が Anatomia から情報を pull) の初の配線。取得経路は HTTP ではなく **CLI spawn**
> (Anatomia web server の常駐を要求せず `anatomia-analyze` skill と同じ経路で疎結合)。

## フロー

```
/api/flow/start (anatomiaProject | anatomiaRepo 指定) ─┐
                                                       ▼
  getAnatomiaMechanics(source)
    ① fetchAnatomiaDomains (client.ts)
         node <binPath> domains list --project <p> --json   (cwd = Anatomia home)
         └ 空なら autoDraft: domains draft → 再 list
    ② refineDomainsToMechanics (refine.ts, LLM 1 コール)
         ドメインカード → 「遊びのメカニクス」だけ抽出 (infra/表示/通信層は除外)
         description を player-facing に書き直し + intended_affect (狙う体験) を補完
                                                       ▼
  buildPaperDraft(theme, tags, { extraMechanics })   ← 先頭に差し込み・名前で重複排除 (先勝ち)
                                                       ▼
  人間レビューゲート (Notion 風編集) → 承認 → runFlow(paperOverride) で議論
```

## 実装 (SRP)

| ファイル | 役割 |
|---|---|
| `src/flow/anatomia/types.ts` | `AnatomiaDomainCard` / `AnatomiaSource` 型のみ。 |
| `src/flow/anatomia/client.ts` | Anatomia CLI アクセス (I/O 境界)。`fetchAnatomiaDomains`。`AnatomiaCliRunner` を DI して spawn をテスト可能に。bin 不在 / 非 0 終了 / JSON 不正は **fail-fast** で throw。 |
| `src/flow/anatomia/refine.ts` | ドメイン→メカニクス精製 (LLM + 決定論フォールバック)。DB/プロセス非依存・単体テスト可。 |
| `src/flow/anatomia/index.ts` | `getAnatomiaMechanics` (取得+精製) + `resolveAnatomiaSource`。 |
| `src/flow/paper-review.ts` | `buildPaperDraft` に `extraMechanics` を追加 + `mergeMechanics` (名前で重複排除)。 |
| `src/flow/web/routes.ts` | `/api/flow/start` が `anatomiaProject`/`anatomiaRepo` を受け、config 有効時に解決して `extraMechanics` に渡す。 |
| `src/flow/web/page.ts` | `/flow` フォームに「Anatomia 事前情報」欄。 |

## 設定 (`flow.anatomia`)

| キー | 既定 | env | 説明 |
|---|---|---|---|
| `enabled` | `false` | `DISCUTERE_ANATOMIA_ENABLED` | opt-in。false なら欄を指定しても無視 (warn)。 |
| `binPath` | `""` | `DISCUTERE_ANATOMIA_BIN` | `bin/anatomia.mjs` の絶対パス。空なら `../Anatomia/bin/anatomia.mjs` を解決。不在なら取得時 fail-fast。 |
| `autoDraft` | `true` | `DISCUTERE_ANATOMIA_AUTO_DRAFT` | domains 下地が無いとき `domains draft` を自動実行。 |
| `refineModel` | `""` | `DISCUTERE_ANATOMIA_REFINE_MODEL` | 精製に使うモデル (空=既定)。 |
| `timeoutMs` | `180000` | `DISCUTERE_ANATOMIA_TIMEOUT_MS` | CLI 1 コマンドのタイムアウト。 |

## degrade / fail-fast の方針

- **取得失敗** (bin 不在 / CLI 非 0 / JSON 不正) は **throw** して UI にエラー表示する。ユーザが
  Anatomia を明示要求して config も有効なのに黙って情報無しで議論を始めない (§7.1 無言フォールバック禁止)。
- **精製の LLM 失敗 / 空応答** は **決定論フォールバック** (実装メカニクススラグを持つドメインだけ
  description のまま採用・`intended_affect` 無し) に degrade。これは capability 劣化 (LLM 不在) で
  あって設定不備ではない。warn に出す。
- **config 無効 + 欄指定**: 無視して warn (機能 OFF)。

## 前提 (Anatomia 側)

- `--project <name>` を使うなら、対象リポを Anatomia に登録しておく (`anatomia project add <name> <repo>`)。
  登録簿は `<Anatomia home>/.anatomia/projects.json`。ブリッジは CLI を **Anatomia repo root を cwd** に
  して起動するのでそこを home として解決する。
- リポ絶対パス (`anatomiaRepo`) なら登録不要 (`--repo` で都度解析)。下地 (`spec/domains`) は
  対象リポ側に書かれる。
- Anatomia 側のコードを変更したら `npm run build` (bin は dist をロードする)。

## 関連

- 実証メモ: [[project_di_anatomia_preinfo_bridge]] / `review/` の実験ログ。
- `docs/paper-review-gate.md` (事前情報が乗るペーパー編集ゲート) / `docs/information-gate.md`。
