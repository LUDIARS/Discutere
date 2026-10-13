type Mechanic = { name: string; source?: string };

export default {
  post: (result: Mechanic[]): true | string => {
    if (!Array.isArray(result)) return "result must be an array";
    if (result.length > 10) return "at most 10 mechanics";
    return result.every((m) => m.name && m.source !== "llm")
      ? true : "theme mechanics must be named data/games entries (never source=llm)";
  },
};
