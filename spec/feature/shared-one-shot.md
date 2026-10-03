# Shared subscription CLI launch

Claude print mode and Codex exec use Lapilli's `@ludiars/one-shot`. Lapilli owns
native executable resolution, model-role defaults and subscription environment.
Discutere owns model-effort parsing, prompts, response envelopes, accounting,
timeouts and output-file cleanup. Codex keeps its narrower environment allowlist,
read-only sandbox and ephemeral sessions. No automatic replay is added.

Discussion default models resolve centrally while config/env model overrides
remain exact. Persistent worker sessions are a separate execution mode.
The existing Lapilli submodule is advanced to the shared launcher's fixed commit.
Revisor's `setup-lapilli` runs before existing dependency setup and checks.
Rollback restores the consumer and its gitlink together. Live inference and
service restarts are not performed by the implementation session.
