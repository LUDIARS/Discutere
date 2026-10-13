type Draft = { mechanics: Array<{ name: string; source?: string }> };
type Deps = { flow?: string };

export default {
  post: async (
    result: { draft: Draft } | Promise<{ draft: Draft }>,
    _theme: string,
    _tags: unknown,
    deps: Deps
  ): Promise<true | string> => {
    const { draft } = await result;
    if (deps?.flow !== "discussion") return true;
    return draft.mechanics.every((m) => m.source !== "llm")
      ? true : "discussion paper must not contain LLM-enriched (hypothesis) mechanics";
  },
};
