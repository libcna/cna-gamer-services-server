---
title: Deployment: bare Linux, systemd, containers and L4 proxying
summary: A boring production layout, secret and filesystem ownership, health checks, firewall policy and deployment evidence limits.
order: 22
---

## Recommended bare Linux layout

Use a dedicated unprivileged account. Install binaries read-only under `/usr/local/bin`, store the
database under `/var/lib/cna-gamer-services` mode 0700, and supply certificate/key through a root-
managed credential path. Keep backups outside the live database directory and copy them off host.

The checked-in systemd unit creates the state directory, loads certificate/key as private systemd
credentials, sets a 65,536 descriptor ceiling, restarts on failure and enables conservative
filesystem/kernel hardening. It keeps AF_INET/AF_INET6/AF_UNIX, because networking and platform
locking need them. Review paths and test `systemd-analyze security`; do not enable a hardening flag
blindly when SQLite/certificate renewal needs a path.

```sh
sudo install -m 0755 build-release/cna-gamer-services-server /usr/local/bin/
sudo install -m 0644 tools/cna-gamer-services-deploy/systemd/cna-gamer-services.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now cna-gamer-services
```

## Firewall and ports

Allow one public TCP port, normally 47831. Control HTTPS, event WSS and relay WSS share it. Keep
diagnostics 47832 and admin web 47833 on loopback. Administration happens through OS login/SSH or a
separately secured plane. Restrict outbound/backup access according to local policy.

## Container path

The Dockerfile is multi-stage: a six-job ccache build then an unprivileged small runtime. Compose
mounts one persistent data volume, certificate/key secrets, a read-only root filesystem, private
tmpfs, descriptor limits and a readiness health check. Edit secret file paths before use; never
COPY secrets into the build context/image.

```sh
docker compose -f tools/cna-gamer-services-deploy/container/compose.yaml build
docker compose -f tools/cna-gamer-services-deploy/container/compose.yaml up -d
```

The database volume must survive container replacement. Back it up through the SQLite online API,
not a volume copy while running.

## L4 proxy

The nginx `stream` example passes TCP unchanged, preserving service TLS and both WebSocket paths.
An upstream may add distributed connection protection. Confirm whether it preserves the actual
client source; a simple TCP proxy normally makes the proxy the peer unless transparent proxying is
configured, causing all players to share per-address admission/sign-in limits.

Do not substitute an HTTP reverse proxy configuration and assume relay works. If TLS terminates
outside the service, certificate expectations, source identity, upgrade timeouts and both WSS paths
must be explicitly qualified.

## Release procedure

1. Run full qualification and record skips/environment boundaries.
2. Take/verify an online backup and retain the old binary/configuration.
3. Stage certificate permissions and run the new binary's `--version`.
4. Stop, replace binary, start and check service logs plus `/readyz`.
5. Run conformance smoke with a scratch/test title and watch errors/latency.
6. Roll back binary only if schema remains compatible; otherwise restore the pre-upgrade backup.

The examples were inspected and local listener behavior tested. Public remote proxy/container
deployment was not independently exercised; qualify the chosen host in staging.
