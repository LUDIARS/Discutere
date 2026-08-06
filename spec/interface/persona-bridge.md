# Persona bridge API

Voluptas が、本人同意と Discord アカウント紐付けを確認した後に、Discutere から本人の
Discord 発話だけを pull する外部境界。機能全体の目的と非目標は
[Voluptas ペルソナブリッジ](../feature/flow/persona-bridge.md) を参照する。

## Persona import pull {#SPEC-PERSONA-BRIDGE-PERSONA-PULL}

`npm run persona:import -- --url <Voluptas export URL>` は専用 Bearer token を
`DISCUTERE_VOLUPTAS_EXPORT_TOKEN` からだけ読み、URL、引数、設定ファイル、ログへ token を
含めない。URL は HTTPS を必須とし、開発用 loopback (`localhost` / `127.0.0.1` / `::1`) に限り
HTTP を許可する。自動 redirect は拒否し、Authorization を別 endpoint へ転送しない。
`x-next-cursor` は同じ明示済み endpoint の query にだけ設定し、最大 100 page で停止する。

## Utterance export {#SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT}

`GET /api/persona-bridge/utterances`

- 認証: 32 文字以上の専用 Bearer token と、`authorId`、audience、expiry、jti を束縛して
  `x-discutere-persona-assertion` で送る Ed25519 authorization assertion の両方を必須とする。assertion は 5 分以内に失効し、jti は
  SQLite で一回限りに消費する。未設定は 503、検証失敗と replay は 401 とする。
- query: `authorId` は Discord snowflake。初回は任意の `since` (ISO 8601 または epoch ms) と
  `limit` (1..1000) を受け取る。2 ページ目以降は応答の `nextCursor` だけを渡し、`since` と
  `cursor` の併用は 400 とする。
- pagination: cursor は `(posted_at, utterance.id)` の排他的な位置を表す。不透明値として扱い、
  同じ timestamp の発話が `limit` を超えても重複や停止なしに次ページへ進める。
- authorization boundary: `utterances` は `sessions` と workspace を含めて join し、Discord ingress
  が作る `title=discord-session:*` かつ `scene=discord:*` の session に属し、`speaker_id` が
  assertion の `authorId` と完全一致する行だけを返す。web-chat、persona、外部取込、他人の発話は
  対象外とする。
- response: `{ ok: true, utterances: [{ text, createdAt }], nextCursor }`。内部 utterance id は
  pagination の ordering metadata にだけ使い、発話オブジェクトへ露出しない。
  `nextCursor` は次ページの可能性がある場合だけ文字列、それ以外は null。cache は
  `private, no-store` とする。内部例外の本文や DB path は応答へ含めない。

## Security and data handling

Bearer token、assertion private key、Discord account 対応表を repository、query、応答、診断ログへ
保存しない。公開鍵だけを secret-managed environment から読み、本文は認可された pull 応答にだけ
含める。任意 URL への redirect、deserialization、filesystem path、command execution はこの API
contract に含めない。
