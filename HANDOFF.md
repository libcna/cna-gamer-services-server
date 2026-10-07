# CNA Gamer Services human handoff

This repository is prepared for one primary C++ maintainer. The public control/relay v1 contracts
remain compatible; no CNA source change, new repository, submodule or large framework is required.

## What changed

- Reproducible ccache CMake presets and a resource-bounded qualification entry point.
- Independent stateful HTTPS/WSS conformance, six-worker load/soak and scratch-only chaos tools.
- WAL-safe validated online backup/restore with end-to-end recovery proof.
- Loopback liveness/readiness/Prometheus diagnostics, bounded request latency/counters, JSON logs,
  `--help` and `--version`.
- A separate authenticated, CSRF-protected, read-only-capable administrator website that delegates
  mutations to the C++ admin CLI.
- systemd, unprivileged container/Compose and nginx TCP-passthrough deployment examples.
- Source-oriented architecture/database/protocol/security/operations/recovery documents and a
  searchable 34-chapter offline handbook with diagrams and 17 labs.
- One compatibility/security fix: the event WebSocket hello now enforces the same exact bounded
  envelope discipline as control and relay handshakes.

The concise engineering record and evidence boundaries are in
[docs/ai-final-infrastructure-campaign.md](docs/ai-final-infrastructure-campaign.md).

## Build

Install the system dependencies listed in README, including `ccache`, then:

```sh
ccache --version
cmake --preset dev
cmake --build --preset dev       # preset caps compilation at six jobs
tools/qualify.sh quick
ccache --show-stats
```

Persistent presets are `dev` (`build-agent`, RelWithDebInfo), `release`, `asan` (ASan+UBSan) and
`tsan`. Developer/release builds use at most six jobs. Sanitizer builds and tests are deliberately
serial because instrumented translation units have a much larger memory peak. Do not routinely
clean build trees or clear ccache.

## Run

Local development only:

```sh
build-agent/cna-gamer-services-server --database /tmp/service.sqlite3 \
  --listen 127.0.0.1 --port 47831 --insecure-loopback --diagnostics-port 47832
```

Production:

```sh
cna-gamer-services-server --database /var/lib/cna-gamer-services/service.sqlite3 \
  --listen 0.0.0.0 --port 47831 \
  --cert /run/credentials/certificate.pem --key /run/credentials/private-key.pem \
  --diagnostics-listen 127.0.0.1 --diagnostics-port 47832 --log-format json
```

The player listener requires TLS unless explicitly insecure on loopback. Diagnostics is always
loopback-only plain HTTP. Run `--help` for every option and `--version` to identify a binary.

## Administer

The C++ CLI is the recovery/script foundation:

```sh
build-agent/cna-gamer-services-admin DATABASE inspect
build-agent/cna-gamer-services-admin DATABASE title TITLE_ID 'Display name'
printf '%s\n' 'initial-password' | \
  build-agent/cna-gamer-services-admin DATABASE user USERNAME GAMERTAG
```

Every command and stdin schema is documented in README and handbook chapter 4. It never offers
arbitrary SQL.

For ordinary browser administration, create a mode-0600 password file and follow
[the admin web guide](tools/cna-gamer-services-admin-web/README.md). It defaults to loopback,
supports `--read-only`, and requires explicit remote opt-in plus TLS.

## Test and qualify

```sh
tools/qualify.sh quick         # build + 19 C++ unit/component suites
tools/qualify.sh normal        # all configured local tests; external CNA tests may skip 77
tools/qualify.sh security      # persistent ASan/UBSan unit/component build
tools/qualify.sh performance   # Release legacy benchmark smoke
tools/qualify.sh full          # all tests + strict offline-documentation build/drift checks
```

All tiers use ccache. Developer/release tests use at most six jobs and sanitizer tests use one. An
exit-code-77 CNA harness skip means unavailable, never pass. Configure the external harness
environment exactly as README describes when CNA builds are available.

Final Linux loopback evidence on 2026-10-07: full qualification configured 46 tests, passed all 29
applicable tests in 48.86 seconds, explicitly skipped 17 unavailable CNA harness tests and failed
none. Serial ASan/UBSan passed 19/19 unit/component tests; focused serial TSan passed 3/3. The
Release benchmark completed steady, login and descriptor-pressure smoke profiles. See the campaign
record for evidence boundaries and tool versions.

## Conformance

```sh
python3 tools/cna-gamer-services-conformance/conformance.py \
  --build build-agent --json /tmp/conformance.json
```

It provisions scratch state through the admin executable and then uses only verified HTTPS/WSS. It
does not read SQLite or require CNA. The maintained suite has 20 smoke cases plus one deliberate
wall-clock ticket-expiry case, and checks that all 59 public dispatcher operations have black-box
coverage. See
[the conformance guide](tools/cna-gamer-services-conformance/README.md).

## Loadlab and soak

```sh
python3 tools/cna-gamer-services-loadlab/loadlab.py --build build-agent --smoke
python3 tools/cna-gamer-services-loadlab/loadlab.py --build build-release \
  --players 32 --workers 6 --seconds 3600 --scenarios soak --json /tmp/soak.json
```

Loadlab supports ordinary, sign-in, refresh, churn, directory, reconnect, host migration, events,
relay traffic shapes and bounded resource sampling. Results are loopback evidence, not public-
Internet capacity. See [its README](tools/cna-gamer-services-loadlab/README.md).

## Chaos

```sh
python3 tools/cna-gamer-services-chaos/chaos.py --build build-agent --json /tmp/chaos.json
```

It cannot accept a database path and signals/corrupts only generated scratch state. Its SIGKILL
results prove process-crash recovery, not power loss or filesystem failure. See
[the chaos boundary](tools/cna-gamer-services-chaos/README.md).

## Observability

```sh
curl --fail http://127.0.0.1:47832/healthz
curl --fail http://127.0.0.1:47832/readyz
curl --fail http://127.0.0.1:47832/metrics
```

Readiness requires expected schema and foreign-key-enabled SQLite. Metrics contain build/schema,
database/table counts, connection/refusal counters, bounded operation/outcome counters and a fixed
latency histogram. They never label players, addresses, titles, sessions or credentials. Human
minute statistics remain the default; `--log-format json` gives bounded structured events. The
complete reference is [docs/operations.md](docs/operations.md).

## Back up and restore

```sh
python3 tools/cna-gamer-services-deploy/backup.py \
  --database /var/lib/cna-gamer-services/service.sqlite3 --output-dir /safe/backups

# Stop the service and restore only to a path that does not exist.
python3 tools/cna-gamer-services-deploy/restore.py \
  --backup /safe/backups/service-TIMESTAMP.sqlite3 \
  --database /var/lib/cna-gamer-services-restored/service.sqlite3 --create-directory
```

The backup uses SQLite's online API and verifies/fsyncs/atomically publishes mode 0600. Retention is
off unless explicitly requested. Restore refuses overwrite. Follow
[docs/disaster-recovery.md](docs/disaster-recovery.md) under stress and keep verified encrypted
copies in another failure domain.

## Deploy

The recommended path is one unprivileged Linux service using the checked-in
[systemd unit](tools/cna-gamer-services-deploy/systemd/cna-gamer-services.service), local
diagnostics and one public TLS port. Container/Compose and nginx raw-TCP examples are in the
[deployment guide](tools/cna-gamer-services-deploy/README.md). They are templates: adjust paths and
certificate automation and qualify the actual staging host.

## Read the code

1. `protocol/v1.md`, `protocol/relay-v1.md`, then golden vectors.
2. `src/Protocol.cpp`; the top/dispatcher/transaction structure of `src/Service.cpp`.
3. `src/Store.cpp` and migrations.
4. One read and one durable mutation, using
   [the implementation map](docs/protocol-implementation-map.md).
5. Directory/invitations/parties, then relay authorization/listener/hub and events.
6. `src/Listener.cpp` for transport, workers, lifetime, diagnostics and shutdown.
7. Corresponding focused, E2E, conformance and chaos tests.

[docs/architecture.md](docs/architecture.md) records thread/ownership boundaries.

## Learn the server

Build the offline handbook:

```sh
python3 tools/cna-gamer-services-docs/site/build.py --output /tmp/cna-docs
python3 -m http.server --directory /tmp/cna-docs 8000
```

Start with chapters 1–6, then sessions/relay (11–15), database/recovery/security/observability
(16–19), and finish with the maintainer workflow, references and labs (24–34). The site is
educational; the protocol Markdown remains normative.

## Important invariants

- One server process owns one database and all associated in-memory realtime authority.
- Credentials, data and directory/relay authority remain title-isolated.
- A mutation and its recorded replay outcome commit together.
- Refresh tokens rotate; replay revokes the family.
- Persistent player-visible state uses FULL durability; reconstructible leases use NORMAL.
- Events are coalesced hints; reads/database are truth.
- Relay sources are server-injected from authenticated membership, never trusted from a frame.
- Admin/diagnostics stay separate from the public player protocol.
- No compatibility change may require a CNA modification.

## Known limitations and things not tested

- Single process/machine/SQLite writer; no horizontal scaling or built-in distributed-DDoS defense.
- Linux loopback was exercised. Public remote Internet, a deployed proxy, macOS and native Windows
  were not tested. Container/systemd examples were not installed on a remote production host.
- The static handbook build, navigation and link checks passed, but this environment exposed no
  browser surface for a visual page review.
- CNA harness binaries and real CNA avatar catalogs were unavailable, so 17 harness tests remain
  explicit exit-77 skips; no result is claimed for them.
- Chaos proves process failure, not OS crash, device failure or physical power loss.
- Certificate replacement requires restart; opaque relay voice policy is cooperatively enforced.

## Worthwhile future work

Only pursue these when the environment or a real need exists: run real CNA harnesses/catalog
fixtures; stage a public remote/L4 deployment; add macOS/native Windows CI; deepen black-box cases
for rare operation boundaries; and preserve comparable before/after capacity results when a measured
bottleneck justifies code change. Avoid speculative distributed rewrites or frontend frameworks.
