---
title: Friends, presence, profiles, messages and reviews
summary: Account-global versus title-local social state, authorization edges, reputation and privacy-aware debugging.
order: 7
---

## Friendship establishment

```diagram
Alice -> Server: friends.add(Bob)
Server -> Bob events: friends hint
Bob -> Server: friends.list
Server -> Bob: incoming request
Bob -> Server: friends.accept(Alice)
Server -> Alice events: friends hint
Alice -> Server: friends.list
Server -> Alice: mutual accepted friendship
```

The friends table stores directed edges. A mutual pair means accepted friendship; a lone edge is a
request. Removing a friend removes the relationship as defined by the operation. Block and
privilege checks apply around communication/profile visibility, so database adjacency alone is not
permission.

## Presence and online state

Presence is title-local rich data: mode and bounded text. Presence status is account activity such
as online, away or busy. “Online” also depends on a recent live access session. Joinability derives
from current directory membership and session policy; it is not a client-controlled boolean.

Friends read current presence; events merely suggest re-reading it. During outage a stale client
snapshot is possible until reconnect/poll, which is why UI should represent presence as advisory.

## Profiles, gamer zone and game defaults

Gamertag, gamer zone, region, motto, picture, reputation and achievement-derived gamerscore form
the profile. Gamer zone and XNA `GameDefaults` are account-global; presence is per title. Game
defaults use a fixed typed key set so arbitrary JSON does not become an unmaintainable profile
store.

Profile viewing and user-content privileges plus bilateral blocks determine what another account
may see. The administrator sets privilege policy; the player manages blocks through the protocol.

## Messages and player reviews

Messages are durable account-to-account text with bounded recipients/content, unread state and
deletion. Communication policy, friendship modes and blocks are checked at send time. A recorded
request ID makes retry return the same outcome and prevents duplicate delivery.

Reviews are one `prefer` or `avoid` edge per reviewer/subject. Reputation is the bounded aggregate
displayed by profile. An avoid review also influences directory matching. The admin web shows only
message counts/read metadata, never private message bodies.

## Debugging sequence

When communication fails, check both accounts exist, title authentication, both block directions,
sender communication privilege and whether a friends-only policy has mutual edges. For missing
presence, check `auth.ping`, title scope and expiry before inspecting event delivery.
