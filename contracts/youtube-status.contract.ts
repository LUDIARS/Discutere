export default {
  post: (result: { present: boolean }, env: NodeJS.ProcessEnv = process.env): true | string => {
    return Object.keys(result).length === 1
      && result.present === Boolean(env.DISCUTERE_YOUTUBE_API_KEY?.trim())
      ? true : "status must contain only presence";
  },
};
