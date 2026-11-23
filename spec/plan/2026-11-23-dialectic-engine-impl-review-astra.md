# Discutere 議論エンジン実装評価（Astra）

**総合評価 2/5。** 論点ごとの決着は記録できるが、応答上限で残った論拠が「守った」扱いになり、撤回済みの根拠まで批准や最終要約を支え得る。最初に修正したいのは投票数よりも、結論が根拠として使える範囲である。

対象は 2026-11-23 の C++ 実装 `e796c5a`。以下は静的レビューで、回数はコード上の論理呼び出し数。実行時のトークン数・料金・モデル品質は未測定。

## 1. 議論の流れ

- **注意｜入口によって適用する仕様が異なる。** 既定は dialectic。通常の discussion / improvement は設定で rounds に切り替わり、代理参加者が解決された経路は dialectic に入る。`discussion.md:8` の rounds 既定という記述は古い。改善フローも dialectic では論点決着後に同じ投票を通るため、OVERVIEW の「改善は機械スコア」をそのまま費用見積もりに使えない。根拠 `native/src/config.cpp:295`、`native/src/flow/dispatch.cpp:114`、`native/src/flow/director.cpp:383`、`native/src/flow/dialectic/driver.cpp:660`。
- **軽微｜基本の順序は仕様どおり。** 承認済み論点を再利用し、なければ分割する。各論点で賛否の主張と論拠を交互に登録し、FIFO で反論・承認を処理する。片側が全滅すれば論破、両側全滅なら結論なし。その他を型分類し、事実照会は支持側が判明しても結論なし、統合候補は両側が批准し、かつ止揚ゲート合格なら止揚となる。代理の両者に対立が成立しない場合は、議論や投票を省く。旧スケジューラの attack/free は現行の応酬で選ばれず、指名文の描画のみ再利用する。根拠 `native/src/flow/dialectic/driver.cpp:253,347,396,456,547,577,628`、`native/src/flow/dialectic/exchange.cpp:9`、`native/src/flow/dialectic/exchange_runner.cpp:119,137,184,236`、`native/src/flow/dialectic/scheduler.cpp:54`、`native/src/flow/dialectic/synthesis.cpp:266`。
- **注意｜未討議の論点は最終入力から落ちる。** 人が承認した論点は収束で打ち切らないが、入口で先頭 5 件に縮める。自動分割側は早期収束すると残りが DB 上 open のまま、最終入力には処理済み settlements だけが渡る。「未討議で残る」というコメントどおりの一覧を最終本文からは復元できない。提案は全予定論点を決着表の行に先に確保すること。根拠 `native/src/flow/dialectic/issues.hpp:14`、`native/src/flow/dialectic/driver.cpp:261,287,319,703,724`、`native/src/flow/dialectic/conclusion.cpp:81`。

## 2. LLM 呼び出しの費用対効果

確定ペーパー再利用時の見積もり。新規ペーパーの調査・増補、バックエンド内部の再試行は別計上する。既定は票 3、応答上限 16、深さ 4、通常判定 haiku、最終判定モデルの指定がなければ通常判定を使用する。根拠 `native/src/flow/flow_setup.cpp:29,59`、`native/src/config.cpp:291,297,302`、`native/src/flow/dialectic/driver.cpp:115`。

| 範囲・役割 | 回数と条件 | 根拠 |
|---|---|---|
| 議論全体 | 人物設定は全員をまとめて 1、論点分割は承認済みなら 0、それ以外は通常 1。最終要約は入力条件により 0、通常 1、JSON 不正時は最大 2 | `native/src/flow/persona_setup.cpp:65,96`、`native/src/flow/dialectic/driver.cpp:260,274`、`native/src/flow/conclusion.cpp:75,87` |
| 論点の入口 | 開幕描画 1、定立生成 2、向き判定 0～2、逆向きの再生成 0～2。代理は先に立場判定 2、対立不成立ならこの 2 回で論点終了 | `native/src/flow/dialectic/driver.cpp:335,347,370,396,418,434` |
| 応酬 | 応答生成 T 回、深さ 1 以上の指名描画 D 回、判定 J 回、締め描画 1 回。D と J はそれぞれ最大 T。構造上不正な反論は機械判定で落ちる。論拠の交互投稿は定立の出力を再利用 | `native/src/flow/dialectic/driver.cpp:456`、`native/src/flow/dialectic/exchange_runner.cpp:111,125,155,174,232,274`、`native/src/flow/dialectic/rule_check.cpp:114` |
| 決着 | 論破・双方全滅は追加 0。それ以外は分類 1。事実なら証拠の有無で照会判定 0～1。その他は候補ごと生成 1、止揚ゲート 0～1、両側批准 2。初回＋修正 2 回で最大 12 回（分類は別） | `native/src/flow/dialectic/driver.cpp:547,566`、`native/src/flow/dialectic/tension.cpp:114,122`、`native/src/flow/dialectic/synthesis.cpp:108,219,233`、`native/src/flow/dialectic/synthesis.hpp:16` |
| 投票 | 候補があれば人数分、既定 3。候補の本文を各票で再送し、レンズと提示順を変える。専用モデル指定なし | `native/src/flow/vote.cpp:30,138,145`、`native/src/flow/dialectic/driver.cpp:656` |

例えば承認済みの通常論点 3 件、各 T=16、D=10、J=16、向き判定 2、再生成なし、全論点で統合候補が初回に全ゲートを通ると仮定する。1 論点は 1+2+2+16+10+16+1+1+4+3＝56 回、議論全体は人物設定・最終要約を含め 170 回。投票削減は 9 回、指名・開幕・締めの定型化は 36 回減る計算。入力長は違うので回数の比を料金の比には使えない。

- **注意｜最初に削れるのは描画。** 指名先・引用元・締めの数値は生成前に確定し、失敗時には元の定型文を返す。ここを直接描画すれば、費用と、集計を言い換えて誤る余地の両方が減る。根拠 `native/src/flow/dialectic/scheduler.cpp:108,117,151`、`native/src/flow/dialectic/exchange_runner.cpp:272`。
- **重大｜投票の削減には依存先の変更が必要。** 投票は論点の決着後に走る。人指定の論点では継続可否にも影響しない。一方、自動論点の 2/3 収束、表示・保存、最終要約の起動条件と入力には効く。論破だけの議論で投票をゼロにすると、勝者発言も批准済み止揚もなく、要約が起動しない。先に決着表から最終出力を作り、早期収束は未応答・新規主張・残論点で再定義し、表示用の投票を選択制にする。1 票だけにすると有効 1 票の集中度が常に 100% になる。根拠 `native/src/flow/dialectic/driver.cpp:642,674,690,704`、`native/src/flow/dialectic/convergence.cpp:9`、`native/src/flow/vote.cpp:166,173`、`native/src/flow/conclusion.cpp:51,75`。
- **注意｜検査は判定対象で選ぶ。** 向き・譲歩・反論の意味判定は次の状態を変えるため小モデルを維持。ID の存在・重複・状態はコードで先に検査する。止揚ゲートは超越した枠、批准は各陣営の根拠保存を問うため同一判定ではない。折衷も記録する要件を維持する場合は、その批准を消すと記録の意味が変わる。新しい意味検査は最終要約に限定し、決着表の転記には使わない。根拠 `native/src/flow/dialectic/rule_check.cpp:114,151`、`native/src/flow/dialectic/synthesis.cpp:108,145,245,266`。

## 3. 発言者の文脈とアーキタイプ

- **注意｜人物像は定立と応酬で厚みが違う。** 指定プール人物から引き継ぐのは氏名・役割・立場・話し方・特徴・モデル等。議題別の価値軸と核主張は一括生成する。定立には両方、応酬には価値軸、両側の定立、対象までの連鎖、論点別の声が入る。ペーパー本文は双方の system に固定され、ゲーム資料があれば共通前提になる。遊び手タイプとゲームの型をそれぞれ識別する必須項目はこの経路にはない。人物プール用の詳細な descriptor が存在しても、討論プロンプトに自動で載るわけではない。根拠 `native/src/flow/persona_cast.cpp:46,58`、`native/src/flow/persona_pool.cpp:313,328`、`native/src/flow/persona_setup.cpp:95,104`、`native/src/flow/dialectic/position.cpp:27,121`、`native/src/flow/dialectic/exchange_prompt.cpp:40,56,67`、`native/src/flow/dialectic/exchange_runner.cpp:145,151`。
- **注意｜提案する受け渡し。** Histrio の人物 ID・版・遊びの動機と判断傾向を開始時に取得し、Di の参加者記録に固定する。ゲームの型と該当メカニクスは出所付きでペーパーに置く。定立・応酬には同じ短い人物記述を使い、例えば短時間復帰を重視する人が、協力プレイの待ち時間をどう評価するかを述べられる形にする。この判断傾向と実際のプレイヤー発言 V は区別する。指定陣営と本人の傾向が矛盾するときの扱いもキャスト時に決める。Histrio は設計参照でありサービス疎通を前提にしていない。根拠 `native/src/flow/dialectic/driver.cpp:137,159,166`、`native/src/flow/dialectic/position.cpp:35`、Histrio `README.md:5`、`spec/feature/virtual-persona-life-script.md:104,176`。
- **重大｜代理への誤帰属を防ぐ。** 代理では軸・核主張の生成対象から代理本人を外し、公開発言の根拠 ID と原文を検査する。仮想人物の脚本や遊び手タイプを本人の観測済み属性として足すと、この保証を壊す。代理には根拠付き情報だけを本人由来として渡し、推測は既存の帰属区分を通す。根拠 `native/src/flow/dialectic/driver.cpp:141,162,299,438`、`native/src/flow/dialectic/exchange_prompt.cpp:61`、`native/src/flow/proxy/attribution.cpp:60,69,76`。

## 4. 結論の信頼性

- **重大｜上限と障害が支持に化ける。** 深さ・ターン上限の cut は contested。しかし子がない根は先に unchallenged、子があり refuted でなければ defended となる。表示も refuted 以外を一括加算する。応答失敗も answered で閉じるため、モデル障害が相手の論証成功と区別できない。P1/P2 では「未回答の根」「途中で切れた連鎖」「応答失敗」を個別に検証すべき。根拠 `native/src/flow/dialectic/exchange.cpp:28,74,91,115`、`native/src/flow/dialectic/exchange_runner.cpp:156,269,276`、`native/src/flow/dialectic/goal.cpp:7,22`。
- **重大｜撤回済み根拠が批准の分子にも分母にも入る。** 生成入力には状態名こそ載るが、許可 ID は全根拠。批准側は状態を落とした本文を読み、全根拠数を分母にする。さらに preservedIds は自陣営 ID なら重複も加算される。根拠が 3 個のとき同じ ID を 2 回返し verdict=accept とすれば 2>1.5 を満たす。生存 ID の集合で分母・分子を計算し、空集合は止揚不可、候補の保存宣言と各側の確認結果も突合する。根拠 `native/src/flow/dialectic/synthesis.cpp:29,82,140,179,200,245`。
- **重大｜決着表が要約の出力を拘束しない。** ラウンド要約には両側の元の主張と区分が並ぶが、根拠別状態は落ちる。最終生成にはその要約に加え、直近 20 発言と勝者発言を投入する。撤回した論拠の発言も勝者候補になり得る。非空の summary が JSON にあれば concluded=true となり、再試行は構文の検査だけ。全論点の決着表を確定出力とし、自由要約に許す根拠を論点 ID と根拠 ID の組で指定する。根拠 ID は論点ごとに pro-G1 等を再利用するためである。根拠 `native/src/flow/dialectic/position.cpp:50`、`native/src/flow/dialectic/driver.cpp:656`、`native/src/flow/dialectic/conclusion.cpp:37,65,95`、`native/src/flow/conclusion.cpp:51,58,78,97`。

## 5. v2 改善設計の判定

| 項目 | 現状・重大度 | 改修判断と根拠 |
|---|---|---|
| P1 打ち切り表示 | 一部・重大 | 上限件数はログのみ。終了理由・未応答の内訳を返し、発話、ペーパー、最終表を同じ集計から描画する。`native/src/flow/dialectic/exchange_runner.cpp:111,272,282` |
| P2 未決状態 | 未実装・重大 | cut を先に判定し、未決だけで勝敗を作らない。深さを奇数にするだけでは偏りの向きが変わる。偶奇両方で同じ未決を要求する。`native/src/flow/dialectic/exchange.cpp:74,91`、`native/src/flow/dialectic/goal.cpp:22`、`native/src/config.cpp:299` |
| P3 結論の前提 | 一部・重大 | 決着区分は入力にある。撤回・未決 ID の排除、重複排除、元の問い・評価基準を維持したかの確認と、全論点表の確定出力が必要。意味検査が失敗したら表だけを残せる。`native/src/flow/dialectic/synthesis.cpp:82,145,200`、`native/src/flow/dialectic/conclusion.cpp:65`、`native/src/flow/conclusion.cpp:97` |
| P4 声番号 | 一部・注意 | 保存 ordinal はあるがロード結果に ordinal を保持せず、検索結果の位置で V 番号を再発行する。保存番号を検索・照会・記録末尾の出所一覧まで運び、同文別出所やフィルター後も元番号を維持する。`native/src/flow/paper_voices.cpp:26,35,47`、`native/src/flow/dialectic/issue_voices.cpp:16`、`native/src/flow/dialectic/driver.cpp:378` |
| P5 定義のずれ | 一部・注意 | 分類は 4 種。事実照会後に止揚・論破ではない旨は既に表示する。定義差の分岐と双方の語義を追加する。現在の n は取得件数なので、支持した証拠の件数や V 番号とは区別する。`native/src/flow/dialectic/tension.cpp:50,123,130`、`native/src/flow/dialectic/driver.cpp:594` |
| P6 同じ人が応答 | 未実装・注意 | 現在は相手陣営の最少発言者。深い連鎖では対象ノードの作者は反対陣営なので、親をたどり、今回の応答側でその連鎖に直近で発言した人物を選ぶ。不在時だけ従来選出。`native/src/flow/dialectic/exchange_runner.cpp:33,118,147` |
| P7 批准の公開 | 一部・注意 | 両側 accept の制約と記録はある。公開する結果行には候補本文しか載らない。各側の判断・確認した有効 ID・修正回を決着表へ写す。追加生成は不要。`native/src/flow/dialectic/synthesis.cpp:245,246,266`、`native/src/flow/dialectic/driver.cpp:628` |
| P8 メカニクス照合 | 一部・注意 | 登壇者にはペーパーがあるが反論判定の入力には対応するメカニクスがない。既存の意味判定に該当資料だけを渡し、資料と矛盾する場合と資料不足を区別する。定立で始まった誤解にも同じ規則が必要。`native/src/flow/dialectic/position.cpp:121`、`native/src/flow/dialectic/rule_check.cpp:63,100` |
| P9 根拠と偏り | 一部・注意 | 声を照合する指示はある。件数・偏りは集計されない。V なしには演繹、メカニクス資料、出所不明があり一括分類できない。採用した声と取得母集団を別々に数え、属性不明を残す。`native/src/flow/dialectic/exchange_prompt.cpp:98`、`native/src/flow/dialectic/issue_voices.cpp:19`、`native/src/flow/dialectic/exchange_runner.cpp:272` |
| P10 版識別 | 未実装・注意 | dialectic と rounds の切替はあるが、決着アルゴリズムの版は設定出力・結論保存にない。新規行に版、旧行には旧版または不明を明示し、同議題比較に用いる。`native/src/config.cpp:637`、`native/src/flow/conclusion.cpp:116` |
| P11 診断・引用 | 一部・軽微 | CLI 異常終了は stderr 先頭のみ。空なら長さ制限・機密除去済みの stdout 診断を残す。引用は既に原文から切るが、先頭の文境界で切れた場合は省略記号も付かない。結論節を含む連続した原文と省略表示にする。`native/src/persona_engine/llm/claude_cli.cpp:139,172`、`native/src/flow/dialectic/scheduler.cpp:123,136,154` |

## 6. コード品質とテスト

- **注意｜純粋な判定と副作用の境界を生かせる。** exchange・goal・convergence は状態と判定を個別に検証しやすい。一方 driver は永続化、通知、進行、投票と最終入力を組み立てる。settlement を安定したデータ境界にし、表示・投票・結論が同じ論拠状態を読む形にする。費用ログは共通投票で flow が discussion 固定なので、改善フローの比較では誤計上にも注意する。根拠 `native/src/flow/dialectic/driver.cpp:204,644,674,719`、`native/src/flow/dialectic/convergence.cpp:7`、`native/src/flow/vote.cpp:142`。
- **重大｜失敗時の判定が非対称。** 定立の生成障害・壊れた JSON は主張を根拠に複製して unchallenged にする。反論の意味判定失敗は ok、譲歩の意味判定失敗は concede、批准失敗は reject。障害で論拠を有効化・撤回できる点が問題。応酬は続けても未検証の根拠は決着表で区別する。根拠 `native/src/flow/dialectic/position.cpp:127,146`、`native/src/flow/dialectic/rule_check.cpp:100,151`、`native/src/flow/dialectic/synthesis.cpp:169`。
- **注意｜回帰は既存 C++ テストに追加できる。** 既に論破・応酬木・批准ゲート・投票の golden と、壊れた応答で driver が完走する試験がある。追加したい判定は、①深さ 4/5 と未着手根の cut が勝敗を変えない、②撤回 ID・重複 ID で批准不可、③無投票の論破を出力し結論なし・未討議の行も残る、④声の順序変更でも同じ V が同じ出所を指す、⑤応答障害を敗北と数えない、の五つ。本文の自由生成には固定応答の偽クライアントを使い、状態・出力表・呼び出し回数を別々に照合する。根拠 `native/tests/flow/w2g_golden_part3_tests.cpp:37,163`、`native/tests/flow/w2g_golden_part4_tests.cpp:85,110,197`、`native/tests/flow/w4p_flow_part4_tests.cpp:47`、`native/src/flow/dialectic/exchange_runner.cpp:156`。

## 改修の優先順位（上位 5 件）

1. **未決と障害を独立した状態にする（P2 → P1）。** 深さ・ターン上限と障害を根、連鎖、集計、表示まで保つ。完了の目安は、最後に話した側を変えても未決が勝ちにならないこと。
2. **決着表と批准を直す（P3・P7）。** 生存根拠の集合を唯一の採用元とし、全予定論点の行、批准の理由、評価基準を保存する。撤回した根拠や重複 ID では結論を支えられないことを確認する。
3. **定型描画を置き換え、投票を結論から切り離す。** 無投票でも論破・未決が正しく出てから、人指定論点の投票を選択制にする。自動論点については残論点数と終了理由を比較する。
4. **声とゲーム資料の照合を可能にする（P4・P5・P8・P9）。** 声番号と出所を固定してから、定義差、メカニクスとの矛盾、標本の偏りを区別する。同議題比較には P10 の版を記録する。
5. **人物とゲームの型を発話へ接続する。** Histrio の版付き人物像とペーパーのゲーム資料を定立・応酬の双方に渡し、P6 で同じ人が自分の反論を継げるようにする。代理本人への帰属は既存の証拠検査に従う。
