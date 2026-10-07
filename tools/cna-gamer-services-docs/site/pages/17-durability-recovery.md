---
title: Durability, backup, restore and disaster recovery
summary: What FULL versus NORMAL protects, WAL-safe online backup, validated restore and a stressed-machine runbook.
order: 17
---

## Durability classes

WAL is enabled. Ephemeral/reconstructible operations use `synchronous=NORMAL`: leases, presence,
reads, heartbeat and request bookkeeping can lose the last moments under physical power loss but
recover after a process crash. Player-visible durable mutations temporarily set
`synchronous=FULL` before their transaction: credentials/revocations, awards, scores, friends,
messages, profile and avatar changes.

This distinction is not permission to weaken a workload for benchmark numbers. Process SIGKILL,
OS crash, filesystem failure and power loss are different events. The chaos suite proves process
crash; hardware guarantees come from SQLite, filesystem/device configuration and backups.

## Online backup architecture

```diagram
Running server -> SQLite WAL: continue serving transactions
Backup tool -> SQLite backup API: consistent page snapshot
Backup tool -> Scratch file: integrity and foreign-key checks
Backup tool -> Filesystem: fsync, atomic rename, mode 0600
Operator -> Off-host storage: encrypt and copy verified backup
```

```sh
python3 tools/cna-gamer-services-deploy/backup.py \
  --database /var/lib/cna-gamer-services/service.sqlite3 \
  --output-dir /safe/backups --retain 14
```

Retention is opt-in and happens only after a new verified backup publishes. Never copy an active
database and ignore its WAL. A same-disk backup is operational convenience, not disaster recovery.

## Restore tutorial

Stop the service. Preserve the current database, `-wal`, `-shm` and lock evidence in a dated
incident directory. Restore into a pathname that does not exist:

```sh
python3 tools/cna-gamer-services-deploy/restore.py \
  --backup /safe/backups/service-20261007T170000Z.sqlite3 \
  --database /var/lib/cna-gamer-services/service.sqlite3
```

The tool validates source and restored copy, fsyncs and atomically publishes mode 0600. It refuses
overwrite. Start the intended binary; schema compatibility is authoritative. Then call hello, sign
in a test account, verify a profile/achievement/friend/message/board sample and monitor errors.

## Disaster runbook

1. Stop traffic and record exact symptoms, binary version and file set; do not experiment on originals.
2. If the machine is lost, rebuild the documented host/user/filesystem and obtain binary plus off-host backup.
3. Replace certificate/key if their confidentiality is uncertain; update DNS/clients only after hostname validation.
4. Validate backup integrity on a recovery copy and restore to a new empty destination.
5. If the current DB is damaged, preserve DB/WAL/SHM together for analysis; never delete the only evidence.
6. If schema is too new, run the matching newer binary or restore a pre-upgrade backup. Do not edit `user_version`.
7. For rollback after migration, restore the pre-upgrade backup; migrations are forward-only.
8. Run readiness, conformance smoke and representative durable-state checks before admitting players.
9. Copy a new verified backup off host and write a concise incident timeline.

`service_backup_restore` automates online population/backup/restore and proves access plus refresh
credentials, profile/defaults, achievement, friendship, message and leaderboard state.
