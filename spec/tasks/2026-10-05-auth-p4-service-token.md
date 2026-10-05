# 認証集約 P4: persona bridge の Cernere service token 受理

- Actio: actio:19a2dca4-7b5f-4129-baa0-f03d5e6f3fcc
- 背景: Corpus `spec/plan/auth-plane-consolidation.md` §6 P4 / Cernere `spec/feature/service-token.md`
- 仕様: [persona bridge interface](../interface/persona-bridge.md)

## 分解

1. [x] PASETO v4.public 検証 (node:crypto、依存追加なし) — `src/cernere-service-token/paseto-v4-public.ts`
2. [x] Cernere 公開鍵の取得・キャッシュ — `cernere-public-keys.ts`
3. [x] service token claims 照合 (kind / exp / aud / scope、sub で分岐しない) — `service-token-verify.ts`
4. [x] `/api/persona-bridge/utterances` を新旧両受理に (assertion は据え置き)
5. [x] 送り側: service token 発行 client (exp-60s キャッシュ) と Voluptas pull のフォールバック
6. [x] env 宣言 (`excubitor.catalog.yaml`) と spec 更新
7. [x] テスト `tests/flow/cernere-service-token.test.ts`

## P5 で消すもの

- `src/api/persona-bridge-routes.ts` の固定トークン照合 (else 節) と `DISCUTERE_PERSONA_BRIDGE_TOKEN`
- `src/flow/voluptas-persona-client.ts` `resolveVoluptasBearer` の固定トークン fallback と `DISCUTERE_VOLUPTAS_EXPORT_TOKEN`
