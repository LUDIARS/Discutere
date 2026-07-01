---
id: monster-strike
title: "モンスターストライク"
genre: "puzzle-action-rpg"
workspace_id: "knowledge"
sources:
  - url: "https://xn--eckwa2aa3a9c8j8bve9d.gamewith.jp/"
    title: "モンスト攻略wiki - ゲームウィズ"
    fetched_at: "2026-06-05"
    attribution: "GameWith"
    excerpt_policy: "summary-only"
  - url: "https://altema.jp/monsuto/"
    title: "モンスト攻略Wiki - アルテマ"
    fetched_at: "2026-06-05"
    attribution: "アルテマ"
    excerpt_policy: "summary-only"
  - url: "https://otakuindustry.biz/archives/1988"
    title: "『モンスターストライク』初心者講座 第1章 基本ルール"
    fetched_at: "2026-06-05"
    attribution: "オタク産業通信"
    excerpt_policy: "summary-only"
mechanics:
  - name: "ひっぱりハンティング(引っ張って弾く)"
    description: "スワイプでモンスターを引っ張り、離して弾く。壁や敵に反射させて当てる物理アクション(ビリヤード/ピンボール風)。片手で完結する手軽な操作。"
    intended_affect: "爽快感・手軽さ"
    intended_valence: "positive"
    intended_aspects: ["fun"]
    intended_emotions: ["joy"]
  - name: "友情コンボ"
    description: "味方モンスター同士がぶつかると発動する固有攻撃/サポート。編成と当てる順番で戦略の幅が大きく広がる。"
    intended_affect: "連携の妙・戦略的満足"
    intended_valence: "positive"
    intended_aspects: ["fun"]
    intended_emotions: ["joy", "trust"]
  - name: "ストライクショット (SS)"
    description: "ターン数チャージで撃つ必殺技。高威力・多彩な効果で逆転要素になる。"
    intended_affect: "爽快な逆転・カタルシス"
    intended_valence: "positive"
    intended_aspects: ["fun"]
    intended_emotions: ["joy", "surprise"]
  - name: "属性相性(火/水/木/光/闇)"
    description: "敵属性に有利な属性で組むとダメージが伸びる。編成の基礎。"
    intended_affect: "編成の計画性"
    intended_valence: "positive"
    intended_aspects: ["fun"]
  - name: "ギミックとアンチアビリティ"
    description: "反重力バリア/ワープ/魔法陣/重力バリア等のギミックに対応するアビリティ持ちを編成するパズル性。クエストごとに最適編成を読む。"
    intended_affect: "攻略の知恵・達成感"
    intended_valence: "positive"
    intended_aspects: ["difficulty"]
    intended_emotions: ["joy", "anticipation"]
  - name: "マルチプレイ(最大4人協力)"
    description: "友達や野良と最大4人で同じクエストを協力攻略。"
    intended_affect: "連帯・賑やかな盛り上がり"
    intended_valence: "positive"
    intended_aspects: ["fun"]
    intended_emotions: ["trust", "joy"]
  - name: "ガチャ(オーブ)"
    description: "オーブを使ってキャラを引く。コラボ頻発。入手難度が攻略の鍵にもなる。"
    intended_affect: "射幸心・期待(賛否の源)"
    intended_valence: "positive"
    intended_aspects: ["price_value"]
    intended_emotions: ["anticipation", "joy"]
  - name: "運極(ラックスキル周回)"
    description: "同キャラを集めてラック99(運極)にするやり込み周回。"
    intended_affect: "収集・達成のやり込み"
    intended_valence: "positive"
    intended_aspects: ["replayability", "content"]
    intended_emotions: ["trust"]
aesthetics:
  - name: "ポップで親しみやすいキャラデザ"
    description: "幅広い層に届く明るい絵柄。"
  - name: "片手操作の爽快感"
    description: "引っ張って弾くだけの軽快さ。"
  - name: "お祭り感(コラボ/イベント頻発)"
    description: "話題作とのコラボが絶え間ない。"
  - name: "ワイワイ協力プレイ"
    description: "4人マルチの賑やかさ。"
  - name: "ガチャ演出の射幸性"
    description: "排出演出が期待を煽る。"
---

# 概要

mixi『モンスターストライク (モンスト)』の設計データ(GameKG)。攻略wiki を典拠に、メカニクスと **意図された情動 (intended_affect)** を構造化。各 mechanic に **運営の想定感情 (intended_valence / intended_aspects)** を明示定義し、アプリレビュー/niconico/YouTube コメント由来の観測情動と突合して **Gap率** を測れるようにしてある。

意図の核は「引っ張って弾くだけの手軽さ・爽快感と、友情コンボ/ギミック対応の戦略性、4人協力の賑やかさ」。一方で **ガチャ/課金 (price_value)** と **高難易度の周回=苦行 (difficulty)** が賛否の中心になりやすく、運営想定(これらも positive に振る設計)と観測(課金ストレス・苦行)のズレ (Gap) が大きく出るタイトル。Steam 非掲載 (モバイル専用) のため observed は niconico/YouTube/website/アプリレビューが主経路。出典は summary-only。
