# YouTube API Key From gcloud

Discutere enables YouTube learning when `DISCUTERE_YOUTUBE_API_KEY` exists in
Infisical. The key is not written to `.env` or process environment variables.
Di reads it directly from encrypted config using `.env.secrets` only as
Infisical bootstrap credentials.

Preferred path:

```powershell
# Start Di, then open:
http://localhost:3100/api/admin/tuning
```

The YouTube API key panel can:

- Save an API key directly to Infisical.
- Read a Google Secret Manager secret via `gcloud` and save it to Infisical.
- Refresh Di's runtime cache without restarting.

CLI fallback:

```powershell
npm run env:youtube:gcloud
```

Defaults:

- Google Secret Manager secret: `discutere-youtube-api-key`
- Google Secret Manager version: `latest`
- Infisical key: `DISCUTERE_YOUTUBE_API_KEY`

Overrides:

```powershell
npm run env:youtube:gcloud -- --secret my-youtube-api-key --project my-gcp-project
npm run env:youtube:gcloud -- --secret my-youtube-api-key --version 3
```

Prerequisites:

- `gcloud` is installed and authenticated.
- The active gcloud account can read the Secret Manager secret.
- `npm run env:setup` has already created `.env.secrets` for Infisical bootstrap.

After the CLI command succeeds, use the tuning UI refresh action to reload Di's
runtime cache without restarting. Information gating and learning add `youtube`
to the automatic crawl sources when the key is available in the runtime cache.
