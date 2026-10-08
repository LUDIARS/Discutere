# 単発のレベル整合性診断

2026-10-08 neco 指示: 技能・事象の組合せによる上振れ/下振れはレベルデザインの問題であることも多いため、`/level-check` のスキルに分ける。Actio: b0350627-148c-4135-bf5b-ef967a0fd794。親担当が設計し、Solが実装する。

## 境界と受入条件

- DI-LC-01: `/mechanics-check` はルール、資源循環、M→D→Aの因果、改訂差のみを扱う。`playModel` を受理しない。数値模型が指定された場合は `/level-check` を使う明示エラーとし、黙って無視したり転送しない。
- DI-LC-02: `/level-check level:<場面本文> baseline:<任意> input:<任意JSON>` を専用入口にする。`LevelCheckRequest` は levelText 必須、baselineText/references/playModel 任意。数値模型がない場合は定性的な場面診断に限り、幅・確率を創作しない。数値計算は前実装の検証済み契約を維持する。
- DI-LC-03: 場面の配置/遭遇/出現条件と適用ルールを模型の前提として明示できるようにする。技能帯、情報、開始状態、時間/試行条件とともに比較条件を固定する。前提未提示はdesign_gapとし、配置差からルールの欠陥を断定しない。
- DI-LC-04: どちらも一回のLLM呼出だけで、議論・学習・KG書込を起動しない。出力トップレベルはlogic_differences/design_gapのみ。数値所見はサーバで計算する。根拠不足はunknown、既存の確率/引用/境界検証を維持する。
- DI-LC-05: 所見のcauseDomainはmechanics/level/both/unknownで原因の所在を表す仮説分類。数値幅だけでは原因を特定できずunknown。bothを含めた既知分類には該当する比較条件と資料根拠が必要。根拠がない分類はunknown化する。因果の証明や実機評価済みと表示しない。

## 実装方針

`src/level-check/` にPlay型、模型検証、play-envelope/play-findingsと専用prompt/toolを移す。共通の入力資料・所見・引用検証・単発LLM呼出に限り `src/design-diagnostic/` に抽出し、levelがmechanicsユースケースを呼ぶ依存は作らない。mechanics固有の型/入口はmechanicsに残す。

専用Discord adapter、command-defs、gateway、index注入、generic router誤配送防止を追加する。失敗時は専用入口で終了し、通常議論へ落とさない。既存のephemeral回答・メンション抑止・サイズ制限を維持する。新HTTP境界や外部URL取得は追加しない。

`skills/level-check/SKILL.md` と架空模型のJSON例を用意し、mechanicsスキルから数値入力を移す。旧mechanics仕様は新境界へ更新し、過去実装を現状と混同させない。domain/test登録は変更対象のみ整備する。

## 検証

数値模型の既存受入（相関、lower方向、技能帯、非網羅、確率0、overflow）をlevel側へ移す。mechanicsはplayModel拒否と数値所見を返さないこと、両入口は登録・失敗処理・議論への非流入、levelは場面条件不足とcauseDomainの根拠不足を検証する。テストは記述し、現在の許可範囲では実行しない。静的型検査・差分検査・Anatomia検証後にRevisorへ提出する。親が設計適合を確認し、通知駆動でマージ反映まで追う。
