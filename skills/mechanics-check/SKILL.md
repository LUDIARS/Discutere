---
name: mechanics-check
description: ルール・資源・MDAの因果を一回だけ比較し、logic_differencesとdesign_gapを返す。場面・配置・予測幅の模型はlevel-checkへ分ける。
---

# 単発のメカニクス整合性診断

企画のルール、資源の生成/消費/循環、M→D→Aの因果、改訂差を比較する依頼に使う。
プロジェクト登録・議論・ペーパー承認・学習・KG書込を起動しない。

Discord `/mechanics-check spec:<企画本文> baseline:<任意の比較本文> input:<任意JSON>`。
spec/baseline/inputは各最大6,000文字。`input` は `{"references":[...]}` のみ。
`playModel` は明示エラーとなり、`/level-check` を使う案内を返す。自動転送しない。
配置・技能・遭遇・予測可能性・上振れ/下振れの診断には `skills/level-check/SKILL.md` を使う。

返答のトップレベルは `logic_differences` と `design_gap` だけ。
論理差分のkindはmda_causality/resource_flow/rule_dependency/revision。
矛盾はcontradiction/inconsistent、根拠・条件不足はunknown。改善議論や総評を加えない。
長い結果はUTF-8のmechanics-check.json添付、ephemeral回答、メンション無効。
LLM未設定、不正JSON、引用不正、上限超過、タイムアウトは明示的な診断失敗。

TypeScriptでは既存の構成済みLLMClientを注入する。

```typescript
import { checkMechanicsConsistency } from "./src/mechanics-check/tool.js";
import type { MechanicsCheckRequest } from "./src/mechanics-check/contracts.js";

const result = await checkMechanicsConsistency(suppliedRequest as MechanicsCheckRequest, {
  llm: configuredLlm,
});
```

公開境界で実行時検証する。LLM invokeは一回、conversationOnly=true、最大60秒。
新しいサービスやHTTP認証境界は作らない。

参照はid/source/revision?/text/evidenceKind/domain?。
evidenceKindはspecification/static_implementation/observation/model_assumption/causal_hypothesis。
domainはmechanicsまたはlevelで、内容の領域を呼出側が明示する。省略時は原因分類に使わない。
仕様・静的実装・観測・模型・原因仮説を区別する。URLだけでは本文を取得しない。
Elegantiaの品質基準は内容を渡し、出所をsourceに記載する。MachinationsやMDAの実行済みとは表示しない。
既存economy-analyzerは副作用を持つグラフ生成器なので起動しない。
specと指定時だけのbaselineが本文の参照ID。spec/level/baseline/playModelは予約IDでreferencesに使えない。

各所見のcauseDomainはmechanics/level/both/unknownで、条件付きの原因仮説。
資料根拠と比較条件がない分類はunknown。bothにはmechanics資料とlevel資料の両方の引用が必要。
配置の差や数値幅だけでルールの欠陥を断定しない。引用があること自体も因果の証明にはならない。

toolの上限: 企画/比較各20,000文字、参照16件/各8,000文字、資料合計64,000文字。
LLM各配列32件、本文2,048文字、条件/未確認各16件・512文字、LLM JSON128KiB。
最終JSONと整形済み添付は512KiB。超過は切り捨てずエラー。

examples/request.jsonとresult.jsonは架空の企画と架空のElegantia資料の構造例で、実行記録ではない。
DiscordではspecTextをspec、baselineTextをbaseline、referencesだけをinputへコピーする。
検証コードはnpm run test:mechanics-check / npm run test:discord-hook。実行許可は作業指示に従う。
正本: spec/feature/mechanics-consistency-check.md、分離境界: spec/feature/level-check.md。
