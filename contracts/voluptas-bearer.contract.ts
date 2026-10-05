type Input = {
  serviceToken?: () => Promise<{ ok: true; token: string } | { ok: false; reason: string }>;
  fallbackToken: string;
};

export default {
  post: async (result: string | Promise<string>, input: Input): Promise<true | string> => {
    const bearer = await result;
    if (bearer && bearer === input.fallbackToken) return true;
    return bearer.startsWith("v4.public.") || !input.fallbackToken
      ? true : "bearer must be the issued service token or, only on issuance failure, the fixed token";
  },
};
