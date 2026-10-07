# Database architecture, durability and recovery

The service uses one SQLite database, one process and one `Store` connection. SQLite is not an
incidental persistence detail: foreign keys, transaction boundaries, schema versioning and the
single runtime authority are core invariants.

## Opening and ownership

`Main.cpp` takes an operating-system lock on `<database>.lock` before constructing the service.
`Store` opens the database read/write/create in FULLMUTEX mode, installs a five-second busy timeout,
enables foreign keys and WAL, then migrates. The admin CLI intentionally does not take the runtime
lock; supported administrative statements can run beside the server through normal SQLite locking.

One process is required because directory admission caches, rate limits, relay routes and event
channels are process memory. Sharing only SQLite between two servers would not share those
authorities.

## Schema and migrations

`PRAGMA user_version` is the schema identity. The current version is 23. `Store::Store` refuses a
larger value with `UNSUPPORTED_DATABASE_VERSION`. It applies every missing migration in numerical
order; each migration has its own `BEGIN IMMEDIATE` transaction and sets its exact version before
commit. If migration N fails, N rolls back and previously committed migrations remain valid; the
next startup resumes from the last committed version.

Never edit a migration that an existing database may already have applied. Add the next migration
and a test that opens both a fresh database and an intentional prior-version fixture.

## Transaction boundaries

Every protocol request runs in an outer `Store::Transaction`. Domain helpers may create nested
transactions, implemented as savepoints. Replay-sensitive requests insert their request ID before
the mutation and store the outcome before the outer commit. Consequently the replay record and the
user-visible mutation survive together or not at all; `AtomicityTests.cpp` kills child processes at
the relevant boundaries and proves recovery.

SQLite constraint failures become stable `CONFLICT`; other storage failures become
`INTERNAL_ERROR` without leaking SQL or paths to clients. The one service mutex serializes all
access to the shared connection.

## WAL and synchronous policy

The connection uses WAL. Replaceable runtime state—leases, directory state, presence and request
bookkeeping—uses `synchronous=NORMAL`. Player-visible durable state—credentials/revocations,
achievements, scores, friends, messages, profile choices and avatars—temporarily switches the
connection to `synchronous=FULL` before its request transaction, then restores NORMAL.

NORMAL remains process-crash safe through WAL, but the newest commits can be lost on sudden power
loss/filesystem failure. FULL is the stronger power-loss class SQLite can request; it is not a
claim about broken storage hardware, controller lies or an untested physical power cut.

## Growth and retention review

| Data | Bound / cleanup |
|---|---|
| Access and refresh credentials | Expired/revoked records pruned at issuance; 32 live families per account |
| Request outcomes | 24-hour retention; 1,000,000/title storage cap and 20,000/account in-memory daily budget |
| Directory sessions/machines | 90-second leases; pruned on directory work; at most 1,024 sessions/title |
| Invitations | One-day retained history, live inbox/title/sender bounds |
| Relay tickets/grants | 60-second unused / one-hour grant bounds; 8/machine and 8,192/title |
| Leaderboard gameplay epochs | 24-hour expiry; at most 16 uncommitted per owner |
| Arbitration rounds | Due rounds resolve on commits; resolved rounds pruned after one day |
| Messages | At most 100 stored per recipient; explicit recipient deletion |
| Friends / blocks / reviews | 256 friend edges, 1,024 blocks and 1,024 reviews per owning account |
| Presence / avatars / party membership | One bounded row per natural account/title or account key |
| Achievements, leaderboard rows, accounts, titles and immutable assets | Durable product data; grows with provisioned/user activity and requires capacity monitoring |

Cleanup is request-driven, so expired rows may remain during a completely idle service. They cease
to authorize behavior at their expiry timestamps even before physical deletion.

## Backup and restore

Never copy an active `.sqlite3`, `-wal` and `-shm` set as an improvised backup. Use
`tools/cna-gamer-services-deploy/backup.py`, which calls SQLite's online backup API and validates the
snapshot. Restore with the paired tool to a non-existing path, start the server against it and
verify protocol-visible state. See [disaster-recovery.md](disaster-recovery.md).

`tests/backup_restore_e2e.py` is executable evidence: it populates credentials, friends, profile
state, achievements, messages and a leaderboard; takes an online backup while the server is live;
restores it; starts a second server on the restored database; and reads the durable state through
the public protocol.
