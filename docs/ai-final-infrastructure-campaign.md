# Human-ownership infrastructure campaign

This is the concise engineering record for the long-running human-handoff campaign. It records
facts and decisions, not agent reasoning. Update it at meaningful milestones.

## Baseline: 2026-10-07

- Branch `next` was clean at `9bd5173` before campaign changes.
- The repository contained no submodules or nested Git repositories.
- The implementation was about 10,000 lines across the protocol specifications, C++ service,
  migrations and tests. Schema version 23 is current.
- Host: Debian Linux, GCC 14.2.0, CMake/Ninja, OpenSSL 3.5.7, SQLite 3.46.1, Python 3.13.5.
- `ccache` 4.11.2 was present. The pre-campaign shared cache reported 9,542 cacheable calls, 6,118
  hits (64.12%) and 3,424 misses.
- A fresh `RelWithDebInfo` Ninja build was configured in `build-agent` with
  `CMAKE_CXX_COMPILER_LAUNCHER=ccache` and built with six jobs. The managed environment made the
  configured `/rv/cnaccache` read-only, so the campaign uses the reusable task cache
  `/tmp/cna-gamer-services-ccache` without changing global configuration. Its first build recorded
  44 cacheable calls, 3 hits and 41 misses, proving that compilation went through ccache.
- The complete baseline CTest run used six jobs. All 23 applicable tests passed, including the TLS
  end-to-end test, relay WSS test, protocol golden-vector tests and benchmark smoke test. Seventeen
  tests that require separately built CNA harnesses were explicitly skipped (exit 77); they were
  not counted as passes. The managed sandbox initially denied loopback sockets, so the authoritative
  run was repeated with local socket permission and completed in 50 seconds.

## Architecture established from source

- One server process owns one SQLite database and the in-memory relay/event authority associated
  with it. An operating-system lock prevents a second server process from splitting authority.
- Two Asio network threads accept, handshake and move bounded frames. Four service workers run
  serialized storage work; two separate workers perform scrypt login work so password derivation
  does not queue ordinary requests.
- `Service` is the serialized protocol/application boundary. `Store` owns the single FULLMUTEX
  SQLite connection, migrations, fixed SQL statements and nested transaction/savepoint mechanism.
- SQLite WAL is enabled. Operational leases and request bookkeeping use `synchronous=NORMAL`;
  player-visible durable mutations temporarily use `synchronous=FULL` before their transaction.
- Control protocol state is persistent. Relay routes and event subscriptions are process-local;
  relay tickets, directory leases and refresh families reconnect persistent authority to new
  process-local channels.
- Events are coalesced hints only. Ordinary reads remain the source of truth.
- Relay datagrams are opaque. The relay replaces caller-controlled source identity with the
  authenticated machine identity and routes only within the grant's title and session.

See [architecture.md](architecture.md) and
[protocol-implementation-map.md](protocol-implementation-map.md) for the maintainable source map.

## Initial findings

- README operational text said schema 20 even though the current implementation is schema 23.
  Corrected as part of the reproducibility milestone.
- The README build examples did not enable ccache or bound parallelism. Presets and
  `tools/qualify.sh` now make the safe path the obvious path.
- The existing tests are extensive but are primarily white-box/in-repository tests. The campaign
  therefore begins the separately packaged black-box suite in
  `tools/cna-gamer-services-conformance/` rather than renaming existing tests.

## Milestone: reproducibility and initial conformance

- Added checked-in CMake presets for developer, release, ASan/UBSan and TSan builds. Every preset
  uses ccache. Developer/release builds use at most six jobs; sanitizer builds use one because their
  instrumented translation units have a materially larger memory peak.
- Added `tools/qualify.sh` with quick, normal, full, security and performance tiers. CTest labels
  separate 19 C++ unit/component tests, local integrations and 17 external CNA-harness tests.
- Added the initial black-box control/relay suite. It provisions scratch fixtures through the admin
  CLI, then uses only verified HTTPS/WSS. Its structured report includes the required host/build/run
  metadata and an explicit loopback evidence boundary.
- Conformance grew to 21 stateful scenario families spanning negotiation/envelopes, complete public
  operation coverage, authentication/logout/refresh replay, profiles, achievements/assets/files,
  local and Ranked leaderboards, complete message/review flows, avatar error/catalog surfaces,
  social/privacy/parties, directory/invitations/local gamers/removal/migration, events and relay
  authority/ticket/framing/reconnect behavior. Access-token expiry is forced through the management
  plane and recovered through public refresh. The default profile also waits for the advertised
  relay-ticket lifetime and proves wall-clock expiry while both machine leases remain alive. A
  source-derived guard checks all 59 dispatcher operations remain represented. The expanded
  loopback run completed 21/21 in 70.0 seconds; the 20-case smoke profile skips only that wait.
- Conformance design found that event-channel authentication accepted unknown fields unlike the
  exact control and relay envelopes. `EventListener` now validates exactly `v`, `id`, `game` and
  `token`, including identifier/token bounds; the WSS regression case proves extra fields close the
  channel.
- Added a dependency-free documentation drift check for local links, fenced JSON, required
  artifacts, migration numbering, `user_version`, `SchemaVersion` and the README schema reference.
- Post-change full CTest: 41 configured tests, 24 applicable passes, zero failures, 17 explicit CNA
  harness skips; six jobs, 49 seconds. Documentation checks and `git diff --check` passed.

## Qualification tiers

- `tools/qualify.sh quick`: ccache build and C++ unit/component suites.
- `tools/qualify.sh normal`: quick build plus every configured CTest, preserving explicit skips.
- `tools/qualify.sh security`: persistent ASan/UBSan build and unit/component suites.
- `tools/qualify.sh performance`: persistent release build and benchmark smoke test.
- `tools/qualify.sh full`: normal qualification plus strict documentation validation. More
  campaign subprojects will be added to this tier only after their smoke checks exist.

Every tier caps build/test parallelism at six. The sanitizer tier is intentionally stricter and
runs both compilation and CTest serially. Build trees and raw logs remain ignored.

## Milestone: recovery, diagnostics and hostile failure coverage

- Added online SQLite backup/restore tools with dry-run, validation, fsync, atomic publication,
  mode 0600 and opt-in retention. `service_backup_restore` proves a live backup and restored
  access/refresh credentials, profile/defaults, achievement, friendship, message and leaderboard.
- Added an opt-in loopback-only diagnostics listener with non-mutating liveness, schema/foreign-key
  readiness and Prometheus text. Metrics use bounded operation/outcome/surface labels and include
  build/schema/database/count, connection/refusal and fixed request-latency histogram data.
- Added human and JSON operational logging, plus compatible `--help` and `--version`. Existing human
  startup output remains unchanged for client harnesses. Source/log tests found no secret-bearing
  logging; passwords, credentials and relay tickets are excluded by design.
- Added scratch-only chaos qualification: SIGTERM, SIGKILL plus reconnect, disconnect/retry
  idempotency, broken/expired/replayed relay tickets, a database lock held beyond busy timeout,
  controlled database-full recovery using a child-only file-size limit, hostile transport,
  second-owner refusal and corrupted-copy rejection. The expanded suite completed 9/9. It
  explicitly does not claim OS-crash, filesystem-fault or power-loss evidence.

## Milestone: load, administration and deployment

- Added a canonical loadlab with a hard six-worker ceiling and ordinary, sign-in, refresh, session
  churn, directory pressure, reconnect, host migration, event, relay and soak profiles. Reports are
  versioned JSON plus optional dependency-free HTML and include build/host/configuration, outcomes,
  throughput, bounded latency samples, relay statistics and periodic process/database resources.
- Added a separate admin web process using only the Python standard library and the C++ admin CLI
  for mutations. It is loopback-first, password-file authenticated, session/CSRF protected,
  privacy-minimizing and optionally read-only; remote bind requires explicit opt-in and TLS. Login
  failures and handler concurrency are bounded, with an automated rate-limit regression test.
- Added readable systemd, multi-stage container/Compose and nginx L4 passthrough examples. Systemd
  credentials and container secrets keep certificate/key material out of arguments/images; the
  diagnostics port remains private. Public remote deployment remains untested and is stated so.

## Milestone: offline maintainer handbook

- Added 34 substantial, interconnected teaching chapters generated from plain Markdown by one
  standard-library script. The offline site has persistent navigation, contents, search,
  previous/next flow, copyable examples, tables and locally rendered SVG sequence diagrams.
- Coverage includes the complete requested information architecture grouped by concept: mental
  model/source tour, every domain family, state machines, control/relay contracts, DB/durability,
  security/observability, deployment/recovery, troubleshooting, safe-change workflows, glossary,
  learning paths and 17 scratch labs.
- The strict checker builds the site in a temporary directory, verifies local HTML assets/links,
  required topic markers, every exported metric and every C++ admin command in addition to existing
  Markdown/JSON/schema checks. No Node dependency, CDN or committed generated output exists.

## Milestone: expanded qualification

- Loadlab smoke exercised all ten maintained profiles with four workers: ordinary, sign-in,
  refresh, session churn, directory pressure, reconnect, host migration, events, relay and soak.
  All completed; expected refresh rate-limit outcomes were classified rather than hidden.
- ASan/UBSan compilation and all 19 unit/component tests ran serially with leak detection enabled;
  19/19 passed. LeakSanitizer cannot operate under the managed ptrace sandbox, so the authoritative
  test run used the same built binaries outside that observation restriction.
- A focused TSan build and serial run covered concurrency lifetime, admission and relay flow; 3/3
  passed. `cppcheck` reported two reviewed coroutine false positives and no actionable defect.
- The static handbook generator built all 34 pages and its strict structure/link/contract checks
  passed. No browser surface was available in this environment, so a visual screenshot review is
  explicitly not claimed.
- Final full Linux loopback qualification configured 46 tests: all 29 applicable tests passed in
  48.86 seconds, 17 unavailable CNA harness tests were explicit skips and none failed. This single
  run included conformance, all ten load smoke profiles, all nine chaos cases, admin-web security,
  diagnostics, backup/restore, protocol golden vectors and strict documentation checks.
- The separate Release benchmark smoke completed at 178 requests/second for its steady profile and
  187 requests/second for its login profile; descriptor pressure left the server alive and a
  subsequent `hello` returned `OK`. These are local comparison data, not capacity promises.
- The reusable task ccache ended with 205/207 compiler calls cacheable, 17 hits and 188 misses. All
  four maintained build trees record `CMAKE_CXX_COMPILER_LAUNCHER=ccache`; the low hit ratio reflects
  the first population of several distinct instrumented/optimized configurations, not bypassed
  caching.
- Final deployment review added a root `.dockerignore`, an explicit C++ runtime package and correct
  UID/GID ownership for a newly initialized container data volume. Compose YAML parsed locally;
  Docker itself was unavailable, so an image build is not claimed.

## Environment limitations

- CNA source and harness executables were unavailable. No CNA test is claimed as passing.
- Public Internet, macOS and native Windows validation were not performed.
- Network qualification is loopback only. Deployment templates were not installed on a production
  host, and the generated handbook could not be visually inspected because no browser was exposed.

## Residual work requiring another environment

Worthwhile residual work—not scope padding—is to run real CNA harness/catalog fixtures, visually
review the generated handbook in a browser, qualify an actual remote/L4 deployment and add native
macOS/Windows builds when those environments become available. Preserve comparable before/after
capacity results only when a measured bottleneck justifies code change.
