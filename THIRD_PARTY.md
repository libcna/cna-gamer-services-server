# Dependencies and licensing

Original server implementation, protocol and tests are licensed under [MIT](LICENCE).
No FNA or Xbox LIVE implementation code or proprietary avatar assets are incorporated.
XNA-facing runtime code belongs to the separate CNA repository and retains its own licensing.

Dependencies are found through CMake/system packages; their licenses are independent:

| Dependency | License | Authoritative notice |
|---|---|---|
| OpenSSL >=3.0 | Apache-2.0 | [OpenSSL licensing](https://openssl-library.org/source/license/index.html) |
| Boost headers/Beast | BSL-1.0 | [Boost license](https://www.boost.org/LICENSE_1_0.txt) |
| nlohmann/json | MIT | [JSON license](https://json.nlohmann.me/home/license/) |
| Python websockets 15.0.1 (integration tests only) | BSD-3-Clause | [websockets license](https://github.com/python-websockets/websockets/blob/15.0.1/LICENSE) |
| SQLite library >=3.38 | Public domain | [SQLite notice](https://www.sqlite.org/copyright.html) |

Preserve dependency notices when packaging their sources or libraries. This MIT grant applies
to original server files; it does not replace the licenses of linked dependencies. Source SPDX
labels consistently identify MIT.

The optional Linux NAT-isolation test executes distribution `slirp4netns` as a separate process
(GPL-2.0-or-later), using its separately licensed libslirp runtime (BSD-3-Clause/Expat notices).
Neither helper code nor libraries are copied into or linked with this MIT service product.
Tested packages are unpacked outside the repository, not redistributed here. Consult the
[slirp4netns license](https://github.com/rootless-containers/slirp4netns/blob/master/COPYING) and
[libslirp package notices](https://gitlab.freedesktop.org/slirp/libslirp) when separately packaging
these test prerequisites. This does not change original server/protocol/test licensing.
