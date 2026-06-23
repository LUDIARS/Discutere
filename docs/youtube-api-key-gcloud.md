# YouTube API Key From gcloud

Discutere enables YouTube learning when `DISCUTERE_YOUTUBE_API_KEY` exists in
Infisical. The key is not written to `.env` or process environment variables.
Di reads it directly from the encrypted config at startup using `.env.secrets`
only as Infisical bootstrap credentials.

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

After the command succeeds, restart Di. On the next boot, information gating adds
`youtube` to the automatic crawl sources when the key is found in Infisical.