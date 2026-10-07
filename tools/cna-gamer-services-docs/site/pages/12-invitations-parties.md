---
title: Invitations, joining friends and parties
summary: Invitation lifecycle, friend/party discovery, private slots, join requests and account-level party state.
order: 12
---

## Session invitation lifecycle

```diagram
Host -> Server: invites.send(session, Bob)
Server -> Bob events: invitations hint
Bob -> Server: invites.list
Server -> Bob: pending invitation
Bob -> Server: invites.accept(invite)
Server -> SQLite: status accepted
Bob -> Server: sessions.joinInvited(invite, participants)
Server -> SQLite: consume authority and join atomically
Server -> Bob: status used, session snapshot
```

Invitations are durable, title-scoped, bounded and expiring. `pending`, `accepted`, `dismissed` and
`used` are meaningful states. Acceptance does not itself consume a session slot; invited join does.
Duplicate active invites converge rather than filling inbox quota.

Joining a friend queries live membership and policy rather than trusting presence text. A party
member can similarly expose a joinable game. Blocks, avoid reviews, invitation-only/private slots
and join-in-progress rules still apply.

## Parties

Parties are account-global groups of at most eight friends. A party has a leader, members and
invitations. It is not a game session and owns no relay route. Party views combine membership with
each member's current presence/joinable session so the Guide can show party sessions.

```diagram
Leader -> Server: parties.invite(friend)
Server -> Friend events: party hint
Friend -> Server: parties.accept
Server -> Members events: party hint
Member -> Server: parties.get
Server -> Member: party members and joinable games
Member -> Server: parties.leave
Server -> Remaining members: party hint or dissolved party
```

## Join requests

A join-friend request is represented using invitation infrastructure but marked as requested. It is
not displayed as a normal invitation sent by a host. This shared storage retains expiry/quota and
transaction rules without confusing the Guide inbox semantics.

## Troubleshooting

For a missing invite, check sender membership/authority, recipient identity, title, block/policy,
expiry and inbox quota. For a party that cannot join, inspect the target's actual directory session,
capacity and allow-join policy—party membership alone is never session authority.
