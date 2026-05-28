import { createPublicKey, verify } from "node:crypto";

export function verifyDiscordInteraction(input: {
  rawBody: string;
  signature: string;
  timestamp: string;
  publicKey: string;
}): boolean {
  const { rawBody, signature, timestamp, publicKey } = input;
  if (!rawBody || !signature || !timestamp || !publicKey) return false;
  const msg = Buffer.from(`${timestamp}${rawBody}`, "utf8");
  const sig = Buffer.from(signature, "hex");
  const raw = Buffer.from(publicKey, "hex");
  // Ed25519 public key SPKI prefix (RFC 8410)
  const spkiPrefix = Buffer.from("302a300506032b6570032100", "hex");
  const key = createPublicKey({
    key: Buffer.concat([spkiPrefix, raw]),
    format: "der",
    type: "spki",
  });
  return verify(null, msg, key, sig);
}

export function interactionToCommand(payload: any): string | null {
  const data = payload?.data;
  if (!data?.name || typeof data.name !== "string") return null;
  const options = Array.isArray(data.options) ? data.options : [];
  const args = options
    .map((o: any) => (typeof o?.value === "string" || typeof o?.value === "number" ? String(o.value) : ""))
    .filter((v: string) => v.length > 0);
  return `/${data.name}${args.length ? ` ${args.join(" ")}` : ""}`;
}
