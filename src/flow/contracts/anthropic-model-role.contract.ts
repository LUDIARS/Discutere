export default {
  post: (result: string, model: string): true | string => {
    if (!result.startsWith("claude-")) return "Anthropic requires a resolved model ID";
    if (model.startsWith("claude-") && result !== model) return "An explicit model ID was changed";
    const override = process.env[`LUDIARS_ONESHOT_MODEL_${model.toUpperCase()}`];
    if (/^(opus|sonnet|haiku|fable)$/.test(model) && override && result !== override) {
      return "The configured role override was lost";
    }
    return true;
  },
};
