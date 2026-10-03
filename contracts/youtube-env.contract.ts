export default {
  post: (result: string | null, env: NodeJS.ProcessEnv = process.env): true | string => {
    return result === (env.DISCUTERE_YOUTUBE_API_KEY?.trim() || null)
      ? true : "injected environment must be the sole source";
  },
};
