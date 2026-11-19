export default {
  post: (result: boolean, spec: string | undefined): true | string => {
    const name = spec?.split("@", 1)[0]?.trim() ?? "";
    const codex = /^(?:astra|sol|luna)$/.test(name) || /^(?:gpt|codex)-/i.test(name);
    return result === codex ? true : "The model was routed to the wrong provider";
  },
};
