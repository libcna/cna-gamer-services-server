# CNA Gamer Services deployment tools

This directory owns production deployment and recovery material. It contains safe online
backup/restore tools, a hardened systemd example, a small multi-stage container build and an nginx
TCP proxy example. These are readable starting points rather than host-specific automation.

## Recommended Linux path

For one host, install the binary and use
[`systemd/cna-gamer-services.service`](systemd/cna-gamer-services.service). Create a dedicated
`cna-gamer-services` system user/group, put the database in `/var/lib/cna-gamer-services`, and use
systemd credentials for the chain and private key. The unit leaves the single public TLS listener
on port 47831 and diagnostics on loopback port 47832. Review every pathname before installation.

The service itself terminates TLS. If an upstream is required for distributed-flood controls, use
an L4/TCP proxy so TLS, WebSockets and source-address admission retain their semantics. The
[`nginx-stream.conf`](nginx-stream.conf) example passes TCP unchanged. It has not been exercised on
a public remote network here; validate it in staging. Do not use an HTTP reverse proxy without
carefully addressing source identity and both WebSocket routes.

Firewall policy: allow the public game port (normally TCP 47831), keep diagnostics 47832 private,
and allow administration only through OS login or a separately secured control plane. There is no
separate relay/event port.

## Container path

[`container/Dockerfile`](container/Dockerfile) builds with ccache and six jobs, then copies only the
runtime into an unprivileged Debian image. [`container/compose.yaml`](container/compose.yaml)
mounts persistent data and certificate/key secrets, uses a read-only root filesystem and health
checks the loopback readiness endpoint. Set the two secret `file:` paths to real files before
starting it. The image seeds a new named volume with ownership for UID/GID 65532. The root
`.dockerignore` keeps local build trees, databases and credential material out of the context.
Never add secrets to the Dockerfile or image build context.

## Online backup

The service may remain running. The tool uses Python's binding to SQLite's online backup API, then
runs `integrity_check` and `foreign_key_check`, fsyncs the completed file and atomically publishes
it. It never copies a live database or WAL file byte-for-byte.

```sh
python3 tools/cna-gamer-services-deploy/backup.py \
    --database /srv/cna-gamer-services/service.sqlite3 \
    --output-dir /srv/cna-gamer-services/backups
```

The command prints one JSON object with the final path, size, schema version and integrity result.
Files are mode 0600. Use `--dry-run` to inspect the target without writing. No backup is deleted by
default. `--retain 14` explicitly opts into pruning older files that match this database's generated
backup naming pattern, and pruning occurs only after a new verified backup is published.

Backups should be copied to a different failure domain and encrypted according to the sensitivity of
account/social data. A local backup on the same disk is not disaster recovery.

## Restore

Stop the service first. The restore tool validates the source, uses the backup API to create a
fresh database, validates that result, fsyncs it and atomically publishes it:

```sh
python3 tools/cna-gamer-services-deploy/restore.py \
    --backup /safe/backups/service-20261007T170000Z.sqlite3 \
    --database /srv/cna-gamer-services-restored/service.sqlite3 \
    --create-directory
```

The destination must not exist. This is intentional: the tool never destroys the only copy of a
damaged or merely suspected database. To restore into the production pathname, stop the server,
move the current database plus any `-wal` and `-shm` sidecars into a dated incident directory, then
restore to the now-empty original pathname. Keep the old files until the restored server is
qualified.

After restore:

1. inspect the JSON integrity/schema result;
2. start the exact intended server build against the restored path—this is the authoritative
   schema compatibility check;
3. call `hello`, sign in with a test/operator account and verify representative durable state;
4. monitor startup and request errors before admitting normal traffic.

`service_backup_restore` automates this sequence with disposable state, including an online backup
while the original server is serving and protocol verification after the restored server starts.
