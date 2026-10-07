---
title: Control operation index
summary: All 59 public v1 operations grouped by authority, state effect, implementation owner and black-box evidence.
order: 30
---

This is the fast navigation layer between a client trace and the source. The normative schemas are
in repository file `protocol/v1.md`; implementation ownership is expanded in the
[architecture/source tour](02-architecture.html). The black-box suite
has an executable guard that fails if any operation in the dispatcher lacks an exercise.

## Negotiation, credentials and profile

| Operations | Authority and state | Implementation / evidence |
|---|---|---|
| `hello` | Anonymous read; capability/version contract. | `Service::dispatch`; hello conformance and golden vectors. |
| `auth.login` | Password; creates access plus refresh family. | `Service::dispatch`, `Authentication.cpp`; auth/refresh conformance. |
| `auth.refresh` | Rotating refresh secret; consumes old and issues new atomically. | `Authentication.cpp`; replay-family conformance and atomicity tests. |
| `auth.ping` | Access token; renews presence and returns current policy. | `Service::execute`; auth/social conformance. |
| `auth.logout` | Access token; revokes its family. | `Service::execute`; logout conformance. |
| `gamer.lookup`, `profile.get` | Authenticated profile read subject to viewer privacy. | `Service::identity`; identity/privacy conformance. |
| `profile.gameDefaults`, `profile.setGameDefaults` | Own account; read/replace account-wide preferences. | `Service::execute`, `Store::gameDefaults`; preference conformance. |
| `profile.setGamerZone` | Own account; replace selected zone. | `Service::execute`; profile conformance. |

## Social, presence and progress

| Operations | Authority and state | Implementation / evidence |
|---|---|---|
| `friends.list` | Own graph plus title presence snapshots. | `Service::execute`; social conformance. |
| `friends.add`, `friends.accept`, `friends.remove` | Relationship mutation, communication policy and hints. | `Service::execute`; friendship/event conformance. |
| `presence.set`, `presence.status` | Own title-rich presence / account status. | `Service::execute`; social/boundary conformance. |
| `achievements.list`, `achievements.award` | Title catalog read / trusted award mutation. | `Service::execute`; progress and replay conformance. |
| `messages.send`, `messages.list`, `messages.read`, `messages.delete` | Account-global inbox with privacy and ownership. | `Service::execute`; full message lifecycle conformance. |
| `reviews.submit` | Own review of another account; `prefer`/`avoid`/`clear`. | `Service::execute`; reputation conformance. |
| `privacy.block`, `privacy.unblock`, `privacy.list` | Own block graph; block ends friendship/pending trust. | `Privacy.cpp`; privacy and party precondition conformance. |

## Assets, avatars and leaderboards

| Operations | Authority and state | Implementation / evidence |
|---|---|---|
| `assets.read` | Signed-in bounded content-hash read. | `Service::execute`; chunk/file-route conformance. |
| `avatars.get` | Signed-in batched read and projection negotiation. | `Avatars.cpp`; null/boundary conformance, deep avatar component tests. |
| `avatars.set` | Own validated description, rate limited. | `Avatars.cpp`; invalid black-box case and catalog-backed component tests. |
| `avatars.catalog`, `avatars.catalogPack` | Imported immutable catalog/pack metadata. | `Avatars.cpp`; empty-catalog black-box cases and import tests. |
| `leaderboards.list`, `leaderboards.definition`, `leaderboards.read` | Definition-driven title reads. | `Leaderboards.cpp`/`Service.cpp`; catalog/page conformance. |
| `leaderboards.game.begin` | Creates owner-bound local gameplay epoch for 1–4 credentials. | `Leaderboards.cpp`; local and Ranked conformance. |
| `leaderboards.game.commit` | Atomically validates/aggregates rows; optional Ranked report. | `Leaderboards.cpp`; local commit plus two-machine arbitration conformance. |
| `leaderboards.game.abort` | Owner cancels an uncommitted epoch. | `Leaderboards.cpp`; abort/retry conformance. |

Raw `GET /cna/v1/files/HASH` is a separate authenticated route, not a control operation. Event and
relay upgrades likewise have their own handshakes.

## Directory and invitations

| Operations | Authority and state | Implementation / evidence |
|---|---|---|
| `sessions.create` | Online account plus complete local participant group; creates host machine/session. | `SessionDirectory.cpp`; player and Ranked conformance. |
| `sessions.find` | Online account; filtered live public snapshots. | `SessionDirectory.cpp`; discovery/boundary conformance. |
| `sessions.get` | Current/former member according to removal semantics. | `SessionDirectory.cpp`; membership/removal/migration conformance. |
| `sessions.touch` | Machine owner; renews machine and directory lease. | `SessionDirectory.cpp`; invitation lifecycle conformance. |
| `sessions.update` | Current host; optimistic revision and state/policy mutation. | `SessionDirectory.cpp`; stale revision and Ranked transition conformance. |
| `sessions.join` | Online local group; public capacity and state policy. | `SessionDirectory.cpp`; directory conformance. |
| `sessions.joinInvited` | Accepted recipient plus group; consumes invitation with membership. | `SessionDirectory.cpp`/`Invitations.cpp`; private/join-friend conformance. |
| `sessions.leave` | Machine owner; removes the whole machine, migrates or ends. | `SessionDirectory.cpp`; migration and cleanup conformance. |
| `sessions.remove` | Host removes a non-host machine. | `SessionDirectory.cpp`; `REMOVED_BY_HOST` conformance. |
| `sessions.addMembers` | Existing machine owner adds authenticated local gamers. | `SessionDirectory.cpp`; local-group conformance. |
| `sessions.relayTicket` | Machine owner plus exact participants; issues one-use secret. | `RelayAuthorization.cpp`; relay conformance. |
| `invites.send`, `invites.list`, `invites.get` | Member send / recipient inbox and inspection. | `Invitations.cpp`; invitation conformance. |
| `invites.accept`, `invites.dismiss` | Recipient state transitions. | `Invitations.cpp`; accept/use/dismiss conformance. |
| `invites.joinFriend` | Friend/party relationship grants a pending request to a joinable game. | `Invitations.cpp`; join-friend conformance. |

## Parties and realtime surfaces

| Operations | Authority and state | Implementation / evidence |
|---|---|---|
| `parties.get` | Own party/invitation view in the calling title. | `Parties.cpp`; party conformance. |
| `parties.invite` | Mutual friend plus communication policy; creates party if needed. | `Parties.cpp`; invite/decline/reinvite conformance. |
| `parties.accept`, `parties.decline`, `parties.leave` | Invitation/member transitions and leadership succession. | `Parties.cpp`; lifecycle conformance and social tests. |

The event channel authenticates exactly `{v,id,game,token}` at `/cna/v1/events`, then emits only
coalesced topic hints. The relay authenticates exactly `{v,id,game,ticket}` at
`/cna/relay/v1`, injects the authenticated source machine into binary frames and routes only within
durable membership. Both are black-box exercised independently of the 59 control operations.

## Safe change procedure

When adding an operation, update the capability contract if optional, exact argument validation,
dispatch set, protocol text, this index, implementation map, focused tests and black-box exercise.
The strict documentation check compares the dispatcher set with conformance `PUBLIC_OPERATIONS`;
it intentionally fails instead of allowing a silently undocumented/unexercised operation.
