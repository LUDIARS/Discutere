type Verdict = { ok: boolean; status?: number; claims?: { kind: string; aud: string; scope: string[]; exp: string } };
type Input = { audience: string; requiredScope: string; nowMs: number };

export default {
  post: async (result: Verdict | Promise<Verdict>, input: Input): Promise<true | string> => {
    const verdict = await result;
    if (!verdict.ok) {
      return verdict.status === 401 || verdict.status === 403 || verdict.status === 503
        ? true : "rejections must be 401, 403 or 503";
    }
    const claims = verdict.claims;
    if (!claims || claims.kind !== "service") return "accepted token must be kind=service";
    if (claims.aud !== input.audience) return "accepted token must match the own storage_slug";
    if (!(Date.parse(claims.exp) > input.nowMs)) return "accepted token must not be expired";
    return claims.scope.includes(input.requiredScope) ? true : "accepted token must carry the endpoint scope";
  },
};
