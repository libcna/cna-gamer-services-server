---
title: Leaderboards, gameplay commits and Ranked arbitration
summary: Definition-driven reads, trusted local gameplay boundaries, report reconciliation and disagreement handling.
order: 9
---

## Definitions and reads

A board definition is scoped by title, key and mode. It declares ascending/descending ordering,
aggregation policy, arbitration, and a fixed typed column schema. Entries cannot smuggle a new
column or change `int32` into `int64`; every value is checked against the definition before SQL.

Read modes support pages, centered views and social restrictions defined by the protocol. Ordering
must remain deterministic across equal ratings, pagination and retries. The Guide lists definitions
through `leaderboards.list` before choosing a board.

## Local gameplay commits

```diagram
Game -> Server: leaderboards.game.begin(participants)
Server -> SQLite: create gameplay boundary
Game -> Server: leaderboards.game.commit(rows)
Server -> SQLite: validate participants, board schema and values
SQLite -> Server: apply aggregation atomically
Server -> Game: committed result
```

The begin/commit pair gives one explicit gameplay identity to related board writes. A retry with the
same request ID cannot apply aggregation twice. `leaderboards.game.abort` terminates an abandoned
boundary. This is still a cooperative game trust model: the server validates schema/authority and
transactionality, not whether a claimed score was humanly achieved.

## Ranked arbitration state machine

Ranked sessions require reports from machines rather than trusting one host's final row. Each
machine reports the same round identity, participants and outcomes. The server holds reports until
the expected machines agree, then commits the reconciled rows once.

```diagram
Host -> Server: begin Ranked gameplay round R
Peer -> Server: begin/observe R
Host -> Server: commit report R
Server -> SQLite: store report, wait
Peer -> Server: commit matching report R
Server -> SQLite: resolve and commit board rows
Server -> Host: final result
Server -> Peer: same final result
```

If reports conflict, authority is not guessed. The round remains unresolved or follows the
documented conflict/abort path. Departures and retries are keyed to the durable round; an internal
failure rolls back the request record so a retry can genuinely run again.

## Debugging Ranked failures

Start with the round/gameplay ID, title and board definition. Confirm every expected machine still
belongs to the exact directory session, report participant sets match, typed columns match, and the
round has not already resolved or aborted. Do not “repair” disagreement by editing leaderboard rows:
that bypasses the evidence the arbitration state machine exists to preserve.

The focused tests are `ArbitrationTests`, `RankedMigrationTests` and the protocol/service suite.
Performance work must not weaken `synchronous=FULL` for committed player-visible board state.
