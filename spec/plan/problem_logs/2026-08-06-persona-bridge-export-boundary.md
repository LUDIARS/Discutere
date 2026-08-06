# Persona bridge export provenance and pagination failures

- Date: 2026-08-06
- Status: fixed in working tree
- Area: persona bridge utterance export
- Severity: P1 (privacy boundary and incremental export correctness)

## Summary

The new persona bridge export could treat a web-chat display name equal to a Discord snowflake as
the consented Discord user's speech. Its timestamp-only continuation also repeated the boundary row
and could remain on the first page forever when more than `limit` rows shared one timestamp. After
the tuple cursor fix, an exact-size final page still advertised a nonexistent continuation, and the
route silently clamped an out-of-contract `limit` instead of rejecting it.

## Evidence

- `src/api/web-chat-routes.ts` accepts client-provided `author` and persists it as `speakerId`.
- The original `readHumanUtterances` filtered only `speaker_id` and did not inspect the owning session.
- The original response returned the last `createdAt` as `nextSince`, while the query used
  `posted_at >= since` and ordered equal timestamps by id.
- The revised route returned `nextCursor` whenever `utterances.length === limit`, without checking
  whether another row existed, and converted every `limit > 1000` to `1000`.

## Regression Context

This is newly introduced behavior in PR #241. The feature specification already excluded anonymous
web-chat speech, so the implementation did not enforce its documented provenance boundary.

## Cause

Identity and transport provenance were conflated in `speaker_id`, and the pagination contract omitted
the ordering tie-breaker (`utterance.id`). The first cursor revision inferred continuation from a full
page rather than reading one bounded lookahead row, and its limit parser implemented clamping that was
not part of the interface contract.

## Fix Requirements

- Join `utterances` to `sessions` and accept only trusted Discord ingress sessions.
- Require both the Discord session title and scene markers so a mutable scene alone cannot confer trust.
- Continue exclusively after `(posted_at, id)` with a validated opaque cursor.
- Read at most `limit + 1` rows, return at most `limit`, and emit a cursor only when the lookahead row exists.
- Reject limits outside `1..1000` with 400 instead of silently changing them.
- Keep assertion, audience, expiry, replay, and Bearer checks default-deny.

## Verification

The registered persona bridge test must insert a numeric web-chat spoof beside a Discord utterance and
show that only the Discord row is exported. It must also page over more than `limit` Discord utterances
sharing one timestamp and observe every id exactly once, verify that an exact-size final page has no
cursor, and verify that `limit=1001` is rejected.

## Follow-up

Revisor CI should run the registered flow suite; no manual product interaction is required for these
database and HTTP boundary cases.
