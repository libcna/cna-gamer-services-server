# Disaster recovery runbook

This runbook favors preserving evidence and obtaining a known-good service over clever in-place
repair. Commands are examples; substitute explicit paths for the affected deployment.

## Machine or binary lost, database available

1. Provision a replacement host with the documented dependency versions and the exact known-good
   server build.
2. Copy the stopped database, certificate chain and private key through a protected channel. Keep
   original copies untouched.
3. Ensure the service account owns the database directory and key; make the key/database mode 0600.
4. Run `sqlite3 service.sqlite3 'PRAGMA integrity_check; PRAGMA foreign_key_check;'` while stopped.
5. Start on a private/loopback listener, verify `hello`, authentication and representative state,
   then admit traffic.

Do not start two service processes against the database. Do not copy a database that is still live;
take an online backup first.

## Backup available

1. Select a backup by creation time and retention policy; keep at least one older generation.
2. Restore to a new path with `tools/cna-gamer-services-deploy/restore.py`.
3. Read the reported integrity and schema version. A successful restore does not prove the chosen
   backup contains the desired point in time.
4. Start the intended server build on the restored path. If it reports
   `UNSUPPORTED_DATABASE_VERSION`, use a newer compatible binary—never lower `user_version`.
5. Verify a test account, title catalog, earned achievement/friend/message or other representative
   durable records before changing traffic.

## Current database damaged

1. Stop the server and prevent automatic restart.
2. Move the database, `-wal`, `-shm` and `.lock` into a dated incident directory on the same
   filesystem. Do not delete them; they may be needed for forensic recovery.
3. Restore the newest verified backup to the now-empty production pathname.
4. Start privately and verify as above.
5. Record the lost time window and any state that may need operator reconciliation.

Do not run destructive SQLite repair commands on the only copy. Work on a duplicate if specialist
recovery is attempted.

## TLS certificate replacement

Install the new full chain and private key with service-account ownership and restrictive mode,
validate that they match, then restart. The current server does not reload certificates in place.
Clients/events/relays reconnect; monitor authentication and reconnect failures.

## Failed upgrade or rollback

Take a verified online backup before every upgrade. Migrations move only forward. If a new binary
migrated the database, an older binary may refuse the newer schema; roll back by restoring the
pre-upgrade backup, not by editing `user_version` or reversing SQL manually. Preserve the failed
post-upgrade database separately for diagnosis.

## Service binary lost

Rebuild from a pinned commit with ccache and `cmake --preset release`, or deploy the retained release
artifact. Run the normal qualification suite before serving. Do not substitute an unqualified
binary merely because it opens the database.
