# 単発の企画メカニクス整合性診断

2026-10-08 neco 指示: 議論ではなく単発でロジックの差分とdesign_gapを返す。
同日追記: 上振れ/下振れ・予測可能性はレベルデザインの問題でもあるため、数値模型と場面条件は /level-check へ分離した。旧版のplayModel受理は現行契約ではない。

## 目的と出力

DI-MCC-01: プロジェクト登録なしで、ルール、資源の生成/消費/循環、M→D→Aの因果、改訂差の不整合を一回で比較する。
議論・ペルソナ・投票・ペーパー承認・学習・収集待ちを起動せず、既存議論やKGへ書き込まない。

DI-MCC-02: 出力トップレベルはlogic_differences/design_gapの二つだけ。
各所見に対象、二つの記述、入力資料の引用、consistent/inconsistent/unknown、causeDomain、比較条件、未確認事項を含める。
矛盾はcontradiction/inconsistent、根拠不足や条件不足はunknown。改善議論や総評を加えない。
causeDomainはmechanics/level/both/unknownという条件付き原因仮説。比較条件と該当領域の資料根拠がない分類はunknown。
bothにはルール資料と場面資料の両方が必要。引用や分類を因果・面白さ・実機確認の証明とは表示しない。

## 入力と単発処理

DI-MCC-03: specText必須、baselineText/references任意。referencesはid/source/revision?/text/evidenceKind/domain?。
domainはmechanics/levelで資料内容の領域を明示し、省略時は原因分類に使わない。
仕様・静的実装・観測・模型仮定・原因仮説を区別する。spec/baselineは供給された本文のID、spec/level/baseline/playModelは参照の予約ID。
Elegantiaの品質基準やMDA/Machinations資料は内容を渡す方式。URLを自動fetchせず、検証器・シミュレーションを実行済みと扱わない。

注入されたLLMClientに一回だけinvokeし、conversationOnly=true、最大60秒。資料内の命令を実行指示にしない。
長さ・件数・型・引用を検証し、超過を切り捨てない。LLM失敗、不正JSON、引用不正は明示失敗。
両側の根拠がない適切判定はunknown化しdesign_gapへ載せる。材料不足を空の適切判定にしない。

DI-MCC-04: playModelは受理しない。指定時は/level-checkを使う明示エラーにし、無視・転送しない。
配置・遭遇・出現・技能・予測幅・上振れ/下振れの評価はspec/feature/level-check.mdの責務。
logic_differences.kindはmda_causality/resource_flow/rule_dependency/revisionのみ。play_envelopeを返さない。
配置差からルール欠陥を断定せず、境界をまたぐ原因は資料不足ならunknown。

## 提供面と既存機能

DI-MCC-05: /mechanics-check spec:<企画本文> baseline:<任意> input:<任意JSON>。
inputはreferencesのみ、6000文字以内。旧入力playModelは分離案内エラー。専用async handlerがdeferReply→一回答で終了し、generic routerやsubmitMessage/dispatchFlowへ流さない。
LLM未設定は明示失敗。長い回答はUTF-8 JSON添付、ephemeral、allowedMentions無効。公開自動投稿や別スレッドは作らない。
TypeScript toolとskills/mechanics-check/SKILL.md、架空のJSON例を提供する。新HTTP境界・新サービスは作らない。

src/flow/spec-analyze.tsは抽出器、src/ludus/economy-analyzer.tsはDB書込・ツール呼出を伴うグラフ生成器なので診断から起動しない。
src/flow/design-gap.tsの20次元感情差とは意味を分ける。観測がないとき負のベースラインを捏造しない。
Elegantia EL-MECHANICS-02/03/05の仕様/実装/観測/模型/原因仮説の区別を保つ。

## 実装と検証

src/mechanics-check/は固有契約・prompt・tool、src/design-diagnostic/は資料・所見・引用検証・単発LLM境界。
src/level-check/と相互のユースケース依存を作らない。Discord adapterを専用登録する。
playModel拒否、数値所見なし、原因分類の根拠不足、引用・JSON・失敗・一回呼出、登録・議論への非流入を検証する。
数値模型の既存検証はlevel側へ移す。今回ローカルで実行するのは静的型検査・差分検査だけ。
テストコードは記述し、実行はRevisorの委託範囲に任せる。サービス操作・実機確認は別途。
