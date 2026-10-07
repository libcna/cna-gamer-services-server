---
title: Session directory, creation, joining and removal
summary: Directory state, machines versus local gamers, join capacity, AddLocalGamer, leases and explicit removal.
order: 11
---

## State model

A directory session belongs to one title and is `player` or `ranked`. It records host account and
machine, lobby/playing state, public/private capacity, eight integer-or-null search properties,
join-in-progress and migration policy, revision and expiry. Machines own one to four local member
accounts; members occupy stable ordinals and public/private slots.

The key distinction is **machine versus gamer**. One CNA process may sign in multiple local gamers,
then join them as one machine. Relay authority names the machine while social/progress authority
names each account. AddLocalGamer adds a participant to an already joined machine; it does not
create a second network route.

## Create and join

```diagram
Host game -> Server: sessions.create(kind, capacity, properties, participants)
Server -> SQLite: session + host machine + members
Server -> Host game: session ID, host machine, revision
Joiner -> Server: sessions.find(filters, localCount)
Server -> Joiner: joinable snapshots
Joiner -> Server: sessions.join(session, participants)
Server -> SQLite: capacity/policy/block checks, machine + members
Server -> Joiner: full membership snapshot
```

Search reserves enough slots for the requested local count and filters exact non-null properties.
Join rechecks everything transactionally because search is only a snapshot. Reviews/blocks and
join-in-progress can make a previously visible result no longer joinable.

Private slots require invitation authority. `sessions.joinInvited` consumes accepted invitation
state with the membership change. A request retry returns the stored result rather than consuming a
second slot.

## Leases, touch and removal

Hosts renew session expiry; every machine renews its machine lease. Pruning removes expired
machines, increments revision and either migrates or closes an orphaned session. Leases are
operational state: NORMAL durability is sufficient because clients refresh them and process-crash
recovery is tested.

`sessions.remove` is host authority to remove another machine/member according to protocol rules.
`sessions.leave` removes the caller's machine. `sessions.addMembers` binds additional authenticated
local participants to the caller's existing machine while rechecking capacity and title.

## Invariants worth tests

- One account has at most one membership per title.
- Member machine IDs refer to a machine in the same session.
- Public/private occupied counts never exceed their slot classes.
- The host machine/account match directory ownership until migration commits.
- Revision changes whenever a visible membership/session property changes.
- Relay grants are invalid once their machine or credential authority disappears.
