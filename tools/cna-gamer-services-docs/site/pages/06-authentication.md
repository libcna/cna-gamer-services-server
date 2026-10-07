---
title: Identity, access tokens, refresh rotation and heartbeat
summary: Authentication state machines, credential lifetimes, replay revocation, title isolation and policy refresh.
order: 6
---

## Sign-in

```diagram
Game -> Server: auth.login(username, password, title)
Server -> SQLite: read salt and verifier
Server -> Scrypt worker: derive candidate
Scrypt worker -> Server: constant-time verifier result
Server -> SQLite: create access session and refresh family
Server -> Game: access token, rotating refresh token, profile/policy
```

The administrator creates accounts; the network has no registration/password-reset operation.
Password verification runs outside the Service lock on two dedicated workers, then credential
issuance returns to serialized storage. This prevents expensive scrypt work from freezing ordinary
requests without allowing concurrent Store use.

An access token is random authority scoped to one title and expires after one hour. The database
stores its hash. A token from title A is unauthenticated in title B even for the same account.
Never log, label or persist plaintext tokens outside the client credential store.

## Refresh rotation

```diagram
Game -> Server: auth.refresh(refresh R1)
Server -> SQLite: validate active family and R1 hash
Server -> SQLite: consume R1, issue R2 and access A2 atomically
Server -> Game: R2 and A2
Attacker -> Server: replay R1
Server -> SQLite: revoke entire family
Server -> Attacker: UNAUTHENTICATED
Game -> Server: auth.ping(A2)
Server -> Game: UNAUTHENTICATED
```

Refresh credentials live for up to 30 days and rotate on every use. The client must replace the old
value only after a successful response. Replay of a consumed value is evidence of copying or a
lost-response ambiguity; the conservative response revokes the family, including access sessions.
A player can sign in again with the password. Past 32 live families, the oldest is signed out so a
client without persistent credential storage cannot permanently lock the account.

## Heartbeat and policy refresh

`auth.ping` proves the access credential is live, refreshes last-seen presence with write
coalescing, returns server time, current privileges and the complete bounded block list. CNA calls
it on its update cadence (normally about 30 seconds). A missing heartbeat makes online presence
age out; it does not delete durable account/social state.

Policy refresh is cooperative for opaque relay voice: the server updates client policy, while the
client applies capture/playback restrictions. It is not immediate server-side inspection of voice
content.

## Incident clues

Many `AUTHENTICATION_FAILED` outcomes suggest wrong deployment credentials or guessing. A sudden
family-wide `UNAUTHENTICATED` after refresh suggests replay, not random access expiry. High login
latency with normal other operations points to scrypt worker saturation; high latency everywhere
points to SQLite/Service contention or host pressure.
