# GLAB conclusion publication

SPEC-DI-GLAB-PUBLICATION-01. Approved by neco, 2026-10-18「この設計で開始」.
Actio: a6669b81-6100-40a3-9c08-f2c010dd40d4.

Di owns publication status. GLAB receives only explicitly publishable conclusions whose audience is `public` (全公開) or `glab` (GLab). Missing policy and `private` are denied. This feature does not create an anonymous internet endpoint.

`conclusion_publication` stores policy against the existing conclusion cache ID, for both legacy gap and flow conclusions. Publication approves the exact title, conclusion and content timestamp hash. A changed revision requires fresh operator approval. Revocation and detail reads use current policy without HTTP caching. Output contains only id/title/conclusion/updatedAt/scope, never transcript, paper supplements or personal identifiers. Latest 200 eligible results are displayed.

Local operators open 公開設定 from the learning conclusion list. Management requires a raw loopback peer, local URL host, same-origin browser requests and JSON writes. GLAB credentials do not grant publication management. The editor submits the viewed content revision; concurrent content changes reject saving.

GLAB server calls `GET /api/integrations/glab/conclusions[?id=...]` with a dedicated Bearer credential. Configure `DISCUTERE_GLAB_CONCLUSIONS_TOKEN` (at least 32 bytes) in Excubitor Vault, injected into Di and GLAB; never put its value in files. Missing credential returns 503. Use HTTPS or same-host loopback transport. Existing unrelated review-trend credentials are not reused.

Implementation is separate from the Ae/GLAB poll and attendance work. Tests in `tests/visualize/conclusion-publication.test.ts` cover defaults, scopes, revocation, changed content, forgery and management boundary. Test execution/service restart needs explicit authorization; submitting to Revisor does not claim deployment validation.
