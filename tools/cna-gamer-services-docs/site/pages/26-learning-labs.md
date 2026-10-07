---
title: Learning paths and hands-on labs
summary: Thirty-minute through multi-day curricula and seventeen scratch-state exercises that build real operational understanding.
order: 26
---

## Choose a path

**Thirty minutes:** chapters 1, 2, 5 and 19; run quick qualification and inspect `/readyz`.

**Half day:** add authentication, sessions, relay, database/recovery and labs 1–6.

**Full day:** read every domain chapter, run conformance/chaos/load smoke and labs 7–15.

**Multi-day deep dive:** read both normative specifications/golden vectors, follow the source order,
run CNA harnesses if available, execute a one-hour soak, practice restore on another directory and
make one test-first documentation-sized change.

## Lab safety setup

Use a new private temporary directory and `build-agent`. Never point labs at production. Keep no
real password or certificate; remove scratch state when finished.

```sh
LAB=$(mktemp -d /tmp/cna-learning.XXXXXX)
chmod 700 "$LAB"
DB=$LAB/service.sqlite3
ADMIN=build-agent/cna-gamer-services-admin
```

## Labs 1–4: foundation

Follow the copy-paste-safe [foundation lab runbook](31-labs-foundation.html).

1. **Build and test.** Run `cmake --preset dev`, build preset and `tools/qualify.sh quick`; explain ccache stats and test labels.
2. **Create a title.** Use `title lab 'Learning Lab'`; inspect only with admin `inspect`, not ad-hoc mutation SQL.
3. **Create two accounts.** Pipe distinct passwords to `user alice Alice` and `user bob Bob`; explain why stdin matters.
4. **Start and negotiate.** Run insecure loopback plus diagnostics, then run conformance in its own scratch environment and inspect hello capabilities.

## Labs 5–8: social and progress

Follow the stateful [social and progress lab runbook](32-labs-social-progress.html).

5. **Friendship.** Use a small protocol client/conformance trace to add, list and accept; observe directional then mutual state.
6. **Presence.** Set rich presence/status, read as the friend, stop heartbeat and reason about expiry versus durable friendship.
7. **Achievement.** Add a catalog definition through JSON stdin, award once, retry the same request ID and confirm one earned row/gamerscore.
8. **Leaderboard.** Define a typed board, seed an admin fixture, run a gameplay begin/commit and test an invalid column type.

## Labs 9–12: sessions and realtime

Follow the authority-focused [session and realtime lab runbook](33-labs-sessions-realtime.html).

9. **Create/join.** Create a four-player migratable session, search from Bob, join and identify account/member/machine IDs.
10. **Invitation.** Make one private slot, send/accept/joinInvited, retry and explain why authority is consumed once.
11. **Relay.** Run loadlab relay smoke with `voice`, then `asymmetric`; compare datagrams/bytes and inspect connection metric.
12. **Host migration.** Run conformance/chaos migration cases; draw graceful leave versus lease-expiry transitions.

## Labs 13–17: ownership

Follow the evidence-focused [operations lab runbook](34-labs-operations.html).

13. **Observe.** Scrape health/readiness/metrics, perform login plus invalid request, locate bounded operation/outcome and latency series; switch JSON logs.
14. **Back up online.** Run the server, mutate state, use `backup.py`, inspect JSON/integrity and copy the result to another directory.
15. **Restore.** Stop a scratch server, restore to a new path, start it and verify access/refresh plus social/progress state; then run the automated recovery test.
16. **Loadlab.** Run ordinary/sign-in/refresh/churn for identical short windows in Release; compare errors and percentiles without claiming Internet capacity.
17. **Chaos.** Run chaos smoke, identify which cases are process-crash evidence, and list the power/filesystem tests it intentionally does not fake.

## Prove understanding

You should be able to explain why one database owner matters, which state is FULL versus NORMAL,
why refresh replay revokes descendants, why events are hints, how machine identity enters a received
relay frame, why an invite does not itself occupy a slot, and how to restore without overwriting the
only damaged copy.

Then pick one error from [troubleshooting](23-troubleshooting.html), trace it through specification,
dispatcher, domain method, SQL tables, unit/E2E/conformance tests and metrics. That is the normal
maintainer workflow in miniature.
