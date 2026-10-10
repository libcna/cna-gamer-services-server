<!-- SPDX-License-Identifier: MIT -->
# Releasing cna-gamer-services-server

*Current as of 0.1.0 (2026-10-10).*

The server follows [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html). A release is
an annotated git tag plus a `CHANGELOG.md` entry — there is no separate release branch, no release
workflow, and nothing is published to a package registry or a container registry.

## Where the version lives

The version is decided in exactly **one** place:

```cmake
# CMakeLists.txt (repository root)
project(cna_gamer_services_server VERSION 0.1.0 LANGUAGES CXX)
set(CNA_GAMER_SERVICES_VERSION_PRERELEASE "")   # e.g. "beta.1"; empty on a final release
```

`project(VERSION …)` accepts numeric components only, so the pre-release identifier sits beside it
and the two are joined into `CNA_GAMER_SERVICES_VERSION_STRING` (`0.1.0`, or `0.2.0-beta.1`).
`CNA_GAMER_SERVICES_VERSION_PRERELEASE` is deliberately a normal variable and not a cache entry: a
cached copy would keep an existing build directory reporting the previous release after a bump.

Everything else derives from that:

| Consumer | How it gets the version |
|---|---|
| C++ code | `#include "CnaService/Version.hpp"` → `CnaService::getVersionString()`, `CNA_GAMER_SERVICES_VERSION_MAJOR`, … |
| `cna-gamer-services-server --version` | prints it |
| The diagnostics listener | `cna_build_info{version="…"}` on `/metrics`, `"version"` in `/readyz` |
| The configure banner | `-- cna-gamer-services-server: version <x.y.z>` |

`CnaService/Version.hpp` is **generated** by `cmake/Version.cmake` from
`cmake/templates/Version.hpp.in` into `<build>/generated/include/CnaService/Version.hpp`, a root the
`cna_service` target publishes, so the server, the admin CLI and the tests include it like any
other `CnaService/` header. Never edit the generated file, and never hard-code the version anywhere
else. `tests/VersionTests.cpp` (`service_version`, label `unit`) checks the header against the
version CMake configured without naming a concrete release, so a bump never edits it.

Two copies are maintained by hand and must be updated as part of a bump:

- `CHANGELOG.md` — the release entry and its link definitions at the bottom.
- `README.md` — the *Version* section.

## Numbers that are *not* the server version

- **The control and relay protocol version** — `v1` in the request envelope, the `/cna/v1` path,
  `protocol/v1.md` and `protocol/relay-v1.md`. It moves only on an incompatible wire change, and
  CNA vendors and drift-checks it separately; a server release does not bump it.
- **`title-minimum-version`** is the oldest *game* version a title accepts. It is a property of a
  title, set by the administrator, and says nothing about the server.
- **The database schema version** advances with each migration under `migrations/`, independently
  of releases.

## Pre-1.0 policy

While the major version is 0, a minor bump may change the public behaviour — the wire protocol
stays `v1` and compatible, but limits, defaults, administration commands and diagnostics may move.
Pre-release identifiers are `alpha.N` → `beta.N` → `rc.N`, ordered as SemVer orders them.

## Cutting a release

1. **Choose the version.** Edit `project(cna_gamer_services_server VERSION …)` and/or
   `CNA_GAMER_SERVICES_VERSION_PRERELEASE` in the root `CMakeLists.txt`.
2. **Write the changelog entry.** Move what is under `## [Unreleased]` into a new
   `## [x.y.z] — YYYY-MM-DD` section in `CHANGELOG.md` and add the link definitions at the bottom.
3. **Update `README.md`** — the *Version* section.
4. **Record the build environment** in the entry. Every dependency is a system package — OpenSSL,
   SQLite, Boost, nlohmann/json, the pinned Python `websockets` of `tests/requirements.txt` — so
   the tag selects none of them:

   ```bash
   printf '%s\n' "$(lsb_release -ds)" "$(g++ --version | head -1)" "$(cmake --version | head -1)" \
       "OpenSSL $(dpkg-query -W -f='${Version}' libssl-dev)" \
       "SQLite $(dpkg-query -W -f='${Version}' libsqlite3-dev)" \
       "Boost $(dpkg-query -W -f='${Version}' libboost-dev)" \
       "nlohmann-json $(dpkg-query -W -f='${Version}' nlohmann-json3-dev)"
   ```
5. **Build and test** in the existing `build-agent/` tree with the repository's own entry point
   (`HANDOFF.md` — ccache, at most six jobs, never a fresh tree):

   ```bash
   export CCACHE_DIR=/rv/cnaccache CCACHE_BASEDIR=/rv
   tools/qualify.sh full
   ```

   The configure output prints `-- cna-gamer-services-server: version <x.y.z>` — check it matches,
   and check `build-agent/cna-gamer-services-server --version`. The gate must be **zero
   warnings, zero errors, zero failures**; the tests that drive real CNA processes skip (exit 77)
   unless CNA's harnesses are configured as `README.md` describes, and a skip is never a pass. A
   release should run them: point the six `CNA_SERVICE_*_HARNESS` variables at the harnesses of
   the CNA release this server is meant for, with `SDL_VIDEODRIVER=offscreen` when that CNA build's
   default renderer needs a window (`README.md` § *Tests*); no display is involved. GitHub's `CI`
   workflow runs the same `full` and `security` tiers on every push, without the harnesses.
6. **Commit** the version-bearing files by explicit name (`CMakeLists.txt`, `CHANGELOG.md`,
   `README.md`), never `git add -A`.
7. **Tag** with a `v` prefix and an annotated tag:

   ```bash
   git tag -a v0.1.0 -m "cna-gamer-services-server 0.1.0"
   ```

   The tag string carries the `v`; `CNA_GAMER_SERVICES_VERSION_STRING` never does.
8. **Push only when the project owner asks**, and push the tag explicitly:

   ```bash
   git push origin develop
   git push origin v0.1.0
   ```

   Never move or recreate a tag once it is pushed; if a pushed release needs a fix, release the
   next patch version instead.
9. **Open the next cycle** by adding an empty `## [Unreleased]` section back to `CHANGELOG.md` if
   step 2 consumed it.
