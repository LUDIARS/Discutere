/** Excubitor supplies Vault secrets when it launches the process. */
export async function getYoutubeApiKey(env: NodeJS.ProcessEnv = process.env): Promise<string | null> {
  return env.DISCUTERE_YOUTUBE_API_KEY?.trim() || null;
}

/** Expose presence only; secret management belongs to Excubitor. */
export function getYoutubeApiKeyStatus(env: NodeJS.ProcessEnv = process.env): { present: boolean } {
  return { present: Boolean(env.DISCUTERE_YOUTUBE_API_KEY?.trim()) };
}
