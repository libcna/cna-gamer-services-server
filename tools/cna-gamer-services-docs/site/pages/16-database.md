---
title: Database architecture and migrations
summary: SQLite ownership, table families, fixed SQL, transaction RAII, WAL and the safe procedure for adding a migration.
order: 16
---

## Ownership and table families

One `Store` owns one FULLMUTEX SQLite handle behind the Service mutex. WAL permits concurrent admin
read/write attempts while the server remains the application writer. A process-level lock beside
the database prevents a second server from splitting relay/event/rate-limit authority.

| Family | Representative tables |
|---|---|
| Identity/credentials | `users`, `sessions`, `refresh_families`, `refresh_credentials` |
| Replay/rate state | `request_ids`, invitation send limits |
| Social/profile | `friends`, `blocks`, `messages`, `player_reviews`, `presence`, `avatars` |
| Title content | `titles`, `achievements`, `assets`, `leaderboards` |
| Progress | `earned`, `leaderboard_entries`, gameplay/arbitration tables |
| Online directory | `directory_sessions`, `directory_machines`, `directory_members`, invitations |
| Realtime authority | `relay_tickets`, ticket members; live sockets stay in memory |
| Parties | `parties`, members and invitations |

SQL text is fixed in C++; untrusted values bind through prepared statements. Foreign keys are on.
Transactions use an RAII object: outer scope is `BEGIN IMMEDIATE`; nested helpers use savepoints;
destruction rolls back unless explicitly committed.

## Startup migration

Store reads `PRAGMA user_version`. A newer version fails closed with
`UNSUPPORTED_DATABASE_VERSION`. Version zero creates the initial schema, then each missing numbered
migration runs in its own immediate transaction and advances exactly to its filename number.

```diagram
Server -> SQLite: open database, enable foreign keys/WAL
Server -> SQLite: read user_version N
Server -> Migration N+1: BEGIN IMMEDIATE, fixed SQL
Migration N+1 -> SQLite: PRAGMA user_version=N+1, COMMIT
Server -> Next migration: repeat until SchemaVersion
Server -> Listener: accept traffic only after success
```

## Add a migration safely

1. Back up representative state and understand which invariant changes.
2. Add `NNN_description.sql`; make it valid from exactly the previous version.
3. End with exactly `PRAGMA user_version=NNN` inside the transaction.
4. Add/configure the migration header in CMake and sequential application in Store.
5. Increment `SchemaVersion`; never reuse or edit the meaning of a shipped migration.
6. Test empty creation, upgrade from prior state, data preservation, failure rollback and too-new refusal.
7. Update database/protocol maps and take a production backup before deployment.

SQLite has limited `ALTER TABLE`; a rebuild migration should create a new constrained table, copy
validated rows, replace it and recreate indexes within one transaction. Do not disable foreign keys
as a casual shortcut.

## Growth and indexes

Request IDs expire after a day and are pruned; directory/invitation leases prune as operations run.
Messages, progress, accounts and assets are durable and can grow by operator/game activity. Watch
`cna_database_bytes`, use fixed count queries, and inspect query plans before adding indexes. An
index accelerates reads but adds write/disk cost; tie it to a demonstrated query.
