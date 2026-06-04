import assert from "node:assert/strict";

import { toSpeakerId, maskedPersonaLabel } from "../../../src/crawler/sources/persona.js";

// 保管層: 公開 ID をそのまま安定アンカー化
assert.equal(toSpeakerId("steam", "76561199058015945"), "ext:steam:76561199058015945");
assert.equal(toSpeakerId("youtube", "UCabc"), "ext:youtube:UCabc");

// 露出層: 決定的・不可逆・公開 ID を含まない
const sid = toSpeakerId("steam", "76561199058015945");
const label1 = maskedPersonaLabel(sid);
const label2 = maskedPersonaLabel(sid);
assert.equal(label1, label2, "同一 speaker_id は同じ表示名 (人物の一貫性)");
assert.ok(label1.startsWith("論者#"), "ペルソナ表示名フォーマット");
assert.ok(!label1.includes("76561199058015945"), "露出名に公開 ID を含まない (逆引き不可)");
assert.notEqual(maskedPersonaLabel(toSpeakerId("steam", "1")), label1, "別人は別表示名");

console.log("ok persona anchor + mask");
