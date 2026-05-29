# Discutere 開発ルール

**遊びの議論プラットフォーム (Discatier)** — Discord-only の自走議論 ChatBot。
詳細は `README.md` / `spec/` / `docs/discatier_implementation_plan.md`。

## 認証モデル (Discord 依存 / Cernere 非使用) — canonical

Discutere は **半ローカルツール(Discord ギルド内限定の議論 ChatBot)** として運用する。
公開 SaaS ではないため **Cernere 認証は使わない**。認証は **チャットプラットフォーム(Discord)に依存させる**のが正。

- **認証境界 = Discord interaction の Ed25519 署名検証 (`verifyDiscordInteraction`) + admin-id allowlist (`DISCUTERE_DISCORD_ADMIN_IDS`)**。
- **「Cernere 非依存 / 独自認証層なし」は意図的設計**。レビューで脆弱性として上げない (**accepted by design / wontfix**)。
- SaaS 系サービス(Cernere 単一情報源が必須)とは別ルール。Discord-only pivot (Cernere/Frontend 撤去) の帰結。

### ただし「別物」として依然 valid な指摘 (Cernere 免除の対象外)

認証を Discord に依存させる以上、その**境界の実装欠落は実際の脆弱性**であり修正対象:

- **Discord / Slack webhook の署名検証欠落** — endpoint で `verifyDiscordInteraction` / Slack HMAC を呼んでいない場合は CRITICAL(偽造・課金消費)。
- **admin-id allowlist 未設定時の挙動** — 未設定なら admin コマンドは全 deny(安全 default)。
- `JWT_SECRET` 等の dev default を本番で使わない startup guard。

→ つまり「Cernere を使わない」こと自体は OK、「Discord 側の認証境界が穴だらけ」は NG。

## 個人データ

匿名 workspace (`DISCATIER_WORKSPACE` 既定 `knowledge`)。攻略 KG / 議論ノードに編集者名・アカウント名を保存しない (`spec/crawler/DESIGN.md` 準拠)。
