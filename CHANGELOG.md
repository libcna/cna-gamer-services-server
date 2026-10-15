<!-- SPDX-License-Identifier: MIT -->
# Changelog

All notable changes to cna-gamer-services-server are documented in this file.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html). While the major version is
0, a minor release may change behaviour — see the pre-1.0 note in
[`docs/releasing.md`](docs/releasing.md). The wire protocol has its own version, `v1`, and does not
move with releases.

## [Unreleased]

### Added

- **Avatar description format 3 and catalog revisions** (CNA catalog v3 revision 2). Format 3
  keeps format 2 and adds eyebrows, ears, earrings, wristwear, gloves and rings (each with a side),
  eye shadow, eyeliner, blush and lips, five more colours, a proportion byte and a second block of
  sixteen face bytes (layout in `protocol/v1.md`). Every description must be canonical: exactly
  the lowest format that says everything it says, counting the item formats the catalog declares.
  `avatars.get` serves a reader of formats 1 or 2 a copy it can read (`projectAvatarDescription`);
  stored avatars never change. A catalog version may be re-imported as a higher `revision` of the
  newest catalog that keeps every item's id, slot and format; the running service picks it up.
  Imports accept contract level (`reader`) 2: item kinds 7-16 and face styles, shared catalog
  textures (`cnaTexture`), per-side parts, alpha-blended parts, hat `hairCover`, `follow`, `hides`,
  32 face controls, anchors, a head surface, outfits and face presets, up to 1,024 files and
  manifests up to 1 MiB.
- `avatars.catalog` also returns `manifestSha256` and `manifestSize`, and leaves the manifest out
  when the caller passes `manifest: false` or it does not fit a message (it is then read by hash
  from the file route). `avatars.get` honours `reader` without a catalog list.

### Changed

- **Avatar catalog v3 is the baseline.** The service accepts avatar catalog versions 3 and later
  only (`BaseCatalogVersion`): catalogs 1 and 2 were CNA development catalogs and are refused by
  `avatar-catalog` import, `avatars.set`, administration, `avatars.catalog` and
  `avatars.catalogPack` (INVALID_ARGUMENT), and ignored in an `avatars.get` catalog list. A later
  catalog is imported only once the baseline is, and keeps every earlier item id in its slot.
  Description formats 1 and 2 are unchanged.
- **Schema 25** adds a catalog `revision` and every catalog item's lowest description `format`
  (existing catalogs are revision 1, their items format 1) and allows item slots 7-16.
- **Schema 24** removes catalog 1 and 2 state from an upgraded database: avatars naming those
  catalogs (the accounts keep everything else), their items, file links and manifests. Shared
  content-addressed files are kept. Triggers refuse such rows in fresh and upgraded databases.

## [0.1.0] — 2026-10-10

First tagged release. The server has been developed since 2026-09-28 and has declared 0.1.0 in
`project()` from the start; this tag names a state of the tree rather than introducing new work, so
the entries below describe what the release contains, not what changed since a previous tag.

### Added

- **The service CNA's `GamerServices` and `Net` talk to**, as one C++23 process over HTTPS and
  WSS with a SQLite database: title-scoped accounts and Guide sign-in (scrypt verifiers, one-hour
  access tokens, rotating 30-day refresh families with replay revocation), gamer profiles and
  pictures, friends, rich presence and online status, achievements with pictures, leaderboards with
  paged/centered/restricted reads, commits at gameplay boundaries and Ranked arbitration, messages
  and player reviews, parties, avatars with catalog packs and per-client negotiation, game defaults,
  gamer zone and reputation, a title's minimum game version, the online session directory with
  invitations, `AddLocalGamer`, `RemoveFromSession` and host migration, an account event channel
  that pushes change hints, and the relay that carries online `NetworkSession` ENet traffic
  (game data and voice alike) over one-use tickets. **It is not Xbox LIVE compatible**: protocol,
  accounts, assets and binaries are CNA's own.
- **The control protocol `v1`** ([`protocol/v1.md`](protocol/v1.md)) and **relay protocol `v1`**
  ([`protocol/relay-v1.md`](protocol/relay-v1.md)) with their golden vectors under
  `protocol/golden/`, which CNA vendors and checks for drift.
- **Operations**: the `cna-gamer-services-admin` CLI (titles, accounts, achievements, leaderboards,
  avatar catalogs, backup and restore, inspection; never arbitrary SQL), a separate authenticated
  read-only-capable administrator website, loopback `/healthz`, `/readyz` and Prometheus
  `/metrics`, JSON logs and a per-minute statistics line, `--help` and `--version`, WAL-safe online
  backup and restore, and systemd, container and nginx TCP-passthrough deployment examples.
- **Hardening from the 2026-09-29 production audit**: service work off the network threads with
  scrypt outside the lock and on its own pool, one fsync per durable change, request IDs only for
  mutations and committed together with their change and outcome, per-address connection and
  sign-in admission with authenticated relays counted apart, persistent HTTPS connections, and an
  accept loop that survives `EMFILE` (`README.md` § *Production audit*).
- **Qualification tooling**: `tools/qualify.sh` tiers (`quick`, `normal`, `security`,
  `performance`, `full`), the black-box conformance suite (20 smoke cases over all 59 public
  operations), the load lab and soak, scratch-only chaos, the strict offline handbook check, and
  the committed benchmark evidence under `benchmarks/`.
- **Release identity**: `CnaService/Version.hpp`, generated by `cmake/Version.cmake` from
  `project(cna_gamer_services_server VERSION ...)` and `CNA_GAMER_SERVICES_VERSION_PRERELEASE`,
  replacing the `CNA_SERVER_VERSION` compile definition. It provides
  `CnaService::getVersionString()`, `getVersionMajor/Minor/Patch()`, `getVersionPreRelease()`,
  `isPreReleaseVersion()` and the preprocessor-testable `CNA_GAMER_SERVICES_VERSION_*` macros;
  `--version`, `cna_build_info` and `/readyz` read it. Covered by `service_version`.
- **GitHub Actions `CI`** (`.github/workflows/ci.yml`): `tools/qualify.sh full` and
  `tools/qualify.sh security` on Ubuntu 24.04 with GCC 14, on every push to `develop`, `next` and
  `main`, every `v*` tag and every pull request. The repository had no CI before.
- `docs/releasing.md`: where the version lives, the numbers that are not the server version, and
  the release checklist.

### Dependencies

Every build dependency is a **system package** — OpenSSL ≥ 3, SQLite ≥ 3.38, Boost ≥ 1.74
(headers, Beast), nlohmann/json ≥ 3.11 — and the integration tests use the Python `websockets`
pinned in `tests/requirements.txt`; the tag selects none of them. The 17 tests that drive real CNA
processes need the harnesses of a CNA build (`README.md` § *Tests*) and skip with exit 77 without
them; a skip is never a pass.

### Known limitations

- Not provided, by decision: Xbox LIVE compatibility of any kind; account self-registration or
  password change over the wire; TrueSkill computation; time windows for the `...Recent`
  leaderboard keys; direct peer-to-peer connections; a store, PartnerToken and title update
  delivery. CNA's `docs/gamer-services-known-limitations.md` lists every limitation, client and
  server.
- One process, one SQLite writer, in-memory relay and event hubs: no horizontal scaling. Floods
  from many addresses need a firewall or L4 filter in front; the per-account request budget
  restarts with the server; a new TLS certificate needs a restart (`README.md` § *Production
  audit*, the *Retained* rows).
- Linux is the tested server host. Windows and macOS builds compile but are not validated as
  servers; on a stock Mac the two load-model smoke tests skip because only `127.0.0.1` is routed.

### Verification

- `tools/qualify.sh full` in `build-agent/` (RelWithDebInfo, ccache, six jobs): all validators
  (module boundaries, audit dispositions, planning database, selective-component matrix parity,
  Unicode tables, GoogleTest accounting, temporary-path policy); Clang 19.1.7 production build of
  all 231 translation units with 0 warnings under `-Werror`; 5 test-only seams with one definition
  each; 55 negative consumer fixtures rejecting all 284 sites; the GCC 14.2.0 build with
  **0 warnings, 0 errors** (65 steps); the strict offline documentation check (56 Markdown files).
- **47/47 tests pass, 0 skipped**, with CNA 0.1.0's six client harnesses from
  `cna/cmake-build-multi` and `SDL_VIDEODRIVER=offscreen` — including the 17 that drive real CNA
  processes, the NAT-isolated ones through slirp4netns 1.2.1, and `service_tls_e2e`'s `award` step
  against a real `GraphicsDevice`. Without the harnesses the same 46 (plus `service_version`)
  report 30 passed and 17 skipped, as CI will.
- The configure banner prints `cna-gamer-services-server: version 0.1.0`,
  `build-agent/cna-gamer-services-server --version` prints the same, and `service_version` holds
  the generated header to it.
- `tools/qualify.sh security`: the AddressSanitizer/UndefinedBehaviorSanitizer build in
  `build-asan/` compiles with 0 warnings and passes all **20/20** unit and component suites
  serially, with LeakSanitizer enabled.
- Verified on Debian GNU/Linux 13 with GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja, OpenSSL
  3.5.7, SQLite 3.46.1, Boost 1.83.0, nlohmann/json 3.11.3, Python 3.13.5 with websockets 15.0.1,
  with CNA at `v0.1.0` (`10a4ef1cb`) supplying the client harnesses.

[Unreleased]: https://github.com/libcna/cna-gamer-services-server/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/libcna/cna-gamer-services-server/releases/tag/v0.1.0
