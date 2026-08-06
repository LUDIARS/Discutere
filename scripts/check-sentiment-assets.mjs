import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// @spec sentiment 空間の一本化

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const pairs = [
  ["src/crawler/sentiment/lexicon.json", "lib/lapilli/packages/sentiment-core/data/lexicon.json"],
  ["data/affects/vocabulary.json", "lib/lapilli/packages/sentiment-core/data/vocabulary.json"],
];

for (const [compatibilityCopy, canonical] of pairs) {
  const left = readFileSync(path.join(root, compatibilityCopy));
  const right = readFileSync(path.join(root, canonical));
  if (!left.equals(right)) {
    throw new Error(`sentiment asset drift: ${compatibilityCopy} differs from ${canonical}`);
  }
}

process.stdout.write("sentiment assets match shared canonical copies\n");
