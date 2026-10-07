# Protocol implementation map

Use this map to move from a wire operation to the code, persistence and regression evidence that
own it. Symbols and file names are preferred over line numbers so the map survives ordinary edits.

| Protocol area | Parser / dispatch | Service and persistence | Focused/E2E tests | Black-box conformance scenario |
|---|---|---|---|---|
| Envelope, hello, versions, errors and limits | `Protocol.cpp`, `Service::dispatch` | `Protocol.hpp`; title minimum version in migration 019 | `ServiceTests.cpp`, protocol golden vectors | hello/capabilities; envelope/version validation; documented boundaries |
| Login, logout, heartbeat, access and refresh | `Service::dispatch`, `Service::execute` | `Authentication.cpp`; `users`, `sessions`, `refresh_families`, `refresh_credentials` | `ServiceTests.cpp`, `AtomicityTests.cpp`, `transport_e2e.py` | authentication/title isolation; refresh replay; logout revocation |
| Request outcome replay | `Service::dispatch` | `request_ids`, migration 021 | `AtomicityTests.cpp` | achievement and message same-ID replay |
| Identity, profile, gamer zone and game defaults | `Service::identity`, `Service::execute` | `users`, migrations 017/018 | `ServiceTests.cpp`, `SocialTests.cpp` | identity/profile/gamer-zone/defaults |
| Friends, presence and voice state | `Service::execute` | `friends`, `presence`, `sessions.voice`, migrations 015/023 | `ServiceTests.cpp`, `SocialTests.cpp`, `PrivacyTests.cpp` | friends/presence/messages/reviews; block trust reset |
| Achievements | `Service::execute` | `achievements`, `earned` | `ServiceTests.cpp`, `ProgressTrustTests.cpp` | achievements/leaderboards/assets/files |
| Assets and raw files | `Service::execute`, `Service::file`, listener GET route | `assets`, `title_assets`, avatar catalog assets | `ServiceTests.cpp`, `PicturePrivacyTests.cpp`, `transport_e2e.py` | asset chunk plus raw authenticated file route |
| Leaderboard catalog/read/local commit | `Service::execute` | `Leaderboards.cpp`; migrations 003/004 | `ServiceTests.cpp`, `NumericPersistenceTests.cpp`, `ProgressTrustTests.cpp` | catalog/read; local commit and abort |
| Ranked arbitration | directory state transition and leaderboard commit | `SessionDirectory.cpp`, `Leaderboards.cpp`; migrations 009/010 | `ArbitrationTests.cpp`, `RankedMigrationTests.cpp` | two-machine Ranked agreement and resolved rows |
| Messages and player reviews | `Service::execute` | `messages`, `player_reviews`, migration 011 | `SocialTests.cpp`, `AtomicityTests.cpp` | send/list/read/delete; prefer/clear reputation |
| Avatars and catalog packs | `Service::avatars` | `Avatars.cpp`, `AvatarAssets.cpp`; migrations 012/014 | `AvatarTests.cpp`, `AvatarValidationTests.cpp`, `cna_avatar_e2e.py` | null reads, empty catalog/pack and invalid description; valid catalog paths remain focused/harness evidence |
| Session directory and host migration | `Service::directory` | `SessionDirectory.cpp`; migrations 006/009/013/016 | `DirectoryTests.cpp`, `ConcurrencyLifetimeTests.cpp`, CNA session harness tests | create/find/get/join/touch/update/add/remove/leave and graceful migration |
| Invitations and join-friend | `Service::invitations`, `sessions.joinInvited` | `Invitations.cpp`; migrations 007/020 | `InvitationTests.cpp`, `SocialTests.cpp`, CNA invitation tests | send/list/get/accept/private join/dismiss/join-friend |
| Parties | `Service::parties` | `Parties.cpp`, migration 020 | `SocialTests.cpp` | get/invite/decline/reinvite/accept/leave |
| Privacy, blocks and privileges | dispatch helpers `mayCommunicate` / `mayView` | `Privacy.cpp`, migration 022 | `PrivacyTests.cpp`, `PicturePrivacyTests.cpp` | block/list/enforcement/unblock and explicit friendship rebuild |
| Event WebSocket | HTTP upgrade in `Listener.cpp` | `EventListener.cpp`; in-memory `EventHub` | `relay_e2e.py`, `SocialTests.cpp` hint sink | exact event hello, authenticated hint and reconnectable close |
| Relay ticket authority | `sessions.relayTicket` | `RelayAuthorization.cpp`, migration 008 | `RelayAuthorizationTests.cpp` | ticket issue, one-use redemption and replay refusal |
| Relay binary framing and routing | WSS upgrade in `Listener.cpp` | `RelayProtocol.cpp`, `RelayFlow.cpp`, `RelayListener.cpp`, `RelayHub.cpp` | relay golden vectors, `RelayProtocolTests.cpp`, `RelayFlowTests.cpp`, `relay_e2e.py` | authenticated source injection, routing/drop and malformed frame close |

## Database migration ownership

`Store::Store` is the only migration runner. `Store::SchemaVersion` must match the highest numbered
migration and every migration must set `PRAGMA user_version` to its own number. CMake embeds SQL into
generated headers at configure time. Adding a migration therefore requires all four changes:

1. add the next `migrations/NNN_name.sql` transaction body;
2. add its `.hpp.in` embedding template and CMake `file(READ)` / `configure_file` entries;
3. increment `Store::SchemaVersion` and add the ordered transactional call in `Store::Store`;
4. add migration-from-old-state and fresh-database tests, then update protocol/operations docs.

Never edit a released migration to change existing databases. Add a new one.
