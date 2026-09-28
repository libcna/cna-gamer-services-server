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
| SQLite library >=3.38 | Public domain | [SQLite notice](https://www.sqlite.org/copyright.html) |

Preserve dependency notices when packaging their sources or libraries. This MIT grant applies
to original server files; it does not replace the licenses of linked dependencies. Source SPDX
labels consistently identify MIT.
