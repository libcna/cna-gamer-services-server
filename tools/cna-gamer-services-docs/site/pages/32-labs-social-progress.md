---
title: Labs 5–8 — friendship, presence and progress
summary: Stateful control-protocol exercises that expose trust transitions, replay behavior and definition-driven progress.
order: 32
---

Continue with the loopback server and variables from the foundation labs. The following helper
sends JSON from stdin so tokens/passwords do not become URL parameters:

```sh
request() {
  curl --silent --show-error --fail-with-body \
    -H 'Content-Type: application/json' --data-binary @- http://127.0.0.1:47831/cna/v1
}
```

## Sign in without exposing passwords

```sh
ALICE_REPLY=$(printf \
  '{"v":1,"id":"lab-login-a","game":"learning","op":"auth.login","args":{"username":"alice","password":"%s"}}' \
  "$ALICE_PASSWORD" | request)
BOB_REPLY=$(printf \
  '{"v":1,"id":"lab-login-b","game":"learning","op":"auth.login","args":{"username":"bob","password":"%s"}}' \
  "$BOB_PASSWORD" | request)
ALICE_TOKEN=$(printf '%s' "$ALICE_REPLY" | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["token"])')
BOB_TOKEN=$(printf '%s' "$BOB_REPLY" | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["token"])')
```

Keep tokens only in this private shell. `unset ALICE_TOKEN BOB_TOKEN ALICE_PASSWORD BOB_PASSWORD`
when finished.

## Lab 5 — friendship is a two-step state machine

Send `friends.add {"gamertag":"Bob"}` with Alice's token. Read Bob's `friends.list {}` and find
`requestReceived:true`, not an accepted edge. Then send `friends.accept {"gamertag":"Alice"}` as
Bob and list from both sides. The invariant is reciprocal accepted rows; one unilateral request
must not disclose private presence.

Use a fresh request ID for each new intent. Repeat the exact accept ID/payload and compare the whole
response: recorded mutation outcomes make a lost-response retry converge. Reuse that ID for
`friends.remove` and observe `DUPLICATE_REQUEST`—correlation IDs are not generic idempotency keys.

## Lab 6 — presence is scoped and eventually offline

As Alice, send `presence.set {"mode":7,"text":"Learning"}` and
`presence.status {"status":"busy"}`, then `auth.ping {"voice":true}`. Bob's next friends read
should show title-scoped text/mode plus chosen busy/voice state while Alice's access session remains
recent. Stop Alice's heartbeat and explain the distinction:

- friendship and selected status are durable account state;
- online/presence visibility depends on a recent title session;
- events are hints, so Bob still reads authoritative state after reconnect.

Do not wait out expiry in a normal edit/test loop; focused tests control time/storage fixtures.

## Lab 7 — catalog versus earned achievement

Create `achievement.json`:

```json
{"key":"tour","name":"Tour","description":"Complete the learning tour","howToEarn":"Finish lab 7","score":25}
```

Import it with `"$ADMIN" "$DB" achievement learning < achievement.json`. Call
`achievements.list`, then `achievements.award {"key":"tour"}` as Alice. Repeat the same request ID
and require the identical response. Send a new request ID: the response now says `awarded:false`,
while the original earned timestamp and gamerscore remain single-valued.

## Lab 8 — typed leaderboard transaction

Create `board.json`:

```json
{"key":"LabScore","mode":0,"ascending":false,"aggregation":"best","arbitrated":false,"columns":{"Rounds":"int32"}}
```

Import it with `leaderboard learning`. Call `leaderboards.game.begin` with kind `local` and Alice's
token in `participants`; retain the returned gameplay ID and Alice's profile `userId`. Commit one
entry with rating 100 and column `{"Rounds":{"type":"int32","value":3}}`, then read the board.

Start a second epoch and deliberately label `Rounds` as `string`: the whole commit must return
`INVALID_ARGUMENT` and write no partial row. Abort that epoch. Explain why admin `seed-leaderboard`
is appropriate for fixtures but is not evidence that gameplay commit authority works.

The executable versions of these flows are in conformance scenarios “friends, presence, messages,
reviews and request replay” and “local leaderboard commit and abort.” Continue with
[session and realtime labs](33-labs-sessions-realtime.html).
