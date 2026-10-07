---
title: Labs 9–12 — sessions, invitations, relay and migration
summary: Exercises for machine-versus-gamer authority and recovery across the control and realtime planes.
order: 33
---

These flows have several opaque IDs and two authenticated actors. Use the black-box conformance
tool as the executable lab client: its source is deliberately small and talks only HTTPS/WSS. Read
the named method before running it, then inspect the JSON result. This teaches the actual request
sequence without introducing a second production client library.

```sh
python3 tools/cna-gamer-services-conformance/conformance.py \
  --build build-agent --json /tmp/cna-session-labs.json
```

## Lab 9 — create, discover, join and add a gamer

Read `Suite.directory_mutation_and_removal`. Draw separate boxes for account, access credential,
machine, member and session. Follow its calls:

1. Alice creates a five-slot player lobby with one non-null search property.
2. Bob discovers it, but cannot `sessions.get` before membership.
3. Bob joins and receives a machine ID distinct from the host.
4. Bob adds Dave as a local gamer; Dave shares Bob's machine but has a separate account/member.
5. Alice enters `playing` with an optimistic revision; replaying the stale revision conflicts.
6. Alice removes Bob's machine, atomically removing both Bob and Dave.

Inspect the case result and the `sessions`/`members` views in admin web while recreating the flow
manually if desired. The essential invariant is that relay routing belongs to the machine while
privacy/progress belongs to accounts.

## Lab 10 — invitation authority

Read `Suite.session_and_invitation` and `Suite.parties_and_join_friend`. Compare three states:

- receipt creates a `pending` row and an event hint;
- explicit `invites.accept` changes it to `accepted` but occupies no slot;
- `sessions.joinInvited` validates the entire local group and consumes it as `used` in the same
  transaction that inserts membership.

Then follow the separate dismissal path and verify a dismissed invite cannot be accepted. Explain
why a private slot is legal for an invited group but an ordinary public join cannot consume it.

## Lab 11 — one-use relay authority and opaque traffic

Read `Suite._relay`, then compare its bytes with `protocol/golden/relay-v1.json`. It obtains a ticket
over control TLS, opens WSS, authenticates exactly once, and sends:

```text
CNR | version | datagram type | reserved | destination machine (16 bytes) | opaque payload
```

The receiving frame's source is injected from the authenticated sender grant; it is never copied
from caller-controlled bytes. The lab proves ticket replay closes, a known destination routes,
an unknown destination drops, and a truncated frame closes with protocol policy.

Exercise traffic shapes without packet logs:

```sh
python3 tools/cna-gamer-services-loadlab/loadlab.py \
  --build build-agent --players 4 --workers 4 --seconds 3 \
  --scenarios relay --packet-shape voice --json /tmp/relay-voice.json
python3 tools/cna-gamer-services-loadlab/loadlab.py \
  --build build-agent --players 4 --workers 4 --seconds 3 \
  --scenarios relay --packet-shape asymmetric --json /tmp/relay-asymmetric.json
```

These are loopback routing/throughput observations, not public-Internet voice quality.

## Lab 12 — graceful migration versus crash recovery

In `Suite.host_migration_and_privacy`, Alice's explicit leave selects Bob's machine, changes host
identity and revision, and keeps the session alive. In the chaos case “SIGKILL active-session
reconnect,” the process vanishes: SQLite WAL restores durable directory authority, while clients
must create new event/relay connections and redeem new tickets.

```sh
python3 tools/cna-gamer-services-chaos/chaos.py \
  --build build-agent --json /tmp/cna-chaos-lab.json
```

Do not call SIGKILL evidence a power-loss test. A process crash does not model kernel cache loss,
controller lies or filesystem corruption. Finish with [operator ownership labs](34-labs-operations.html).
