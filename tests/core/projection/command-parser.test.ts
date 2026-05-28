import assert from "node:assert/strict";
import { parseCommand } from "../../../src/core/projection/command-parser.js";

assert.equal(parseCommand("hello"), null);
assert.deepEqual(parseCommand("/define Jump Arc"), { command: "define", args: ["Jump", "Arc"] });
assert.deepEqual(parseCommand(" /SUBMIT   "), { command: "submit", args: [] });
console.log("command-parser passed");
