<!-- SPDX-License-Identifier: MIT -->
# Online guests v1

Owner-approved CNA replacement-service policy; native Xbox credentials, Ranked
admission and guest accounting remain unqualified. Implemented against CNA
4de39f871 and server ed0008b, on top of committed GAP-17/GAP-18. Schema27 is
approved; migration026 is unchanged. No deployment is claimed. CNQ/CNR stay v1.

## Identity and credentials

A guest is a service-issued participant, never a row in `users`, an account,
a password, a refresh token, or a valid top-level request token. Its identifier
is `g:` followed by 32 lowercase hexadecimal digits (16 cryptographically random
bytes). The prefix separates it from every account ID. Possessing that public
identifier grants nothing. Admission always presents a current **parent access
credential** and verifies the same title, parent account, and live refresh
family that opened the guest. Access rotation within that family preserves the
identity. A new login family cannot inherit it, even for the same account.
The word parent means the backing local account, not necessarily the network
session host. A guest cannot parent a guest or be a machine owner/session host.

`guests.open {slot:1|2|3}` with an authenticated account token returns exactly
`{guestId,parentId,gamertag,slot,allowOnlineSessions}`. Slot is a parent-relative
guest number, independent of PlayerIndex; it is not supplied as a global user
ID. Repeating open in the same family/slot returns the same identity while live.
The server derives the name `parentGamertag + " (n)"`, truncating the parent to
28 ASCII bytes if necessary (existing account gamertags satisfy identifier()).
Names must be unique in a session; a collision from another active family of the
same parent is INVALID_STATE, never silently renamed or merged. Normal account
names cannot contain the generated parentheses/space suffix. No caller name,
privilege, title, parent or family override is accepted.

`guests.close {guest:<guestId>}` with that same parent family removes the guest
from its session and deletes the identity atomically; returns `{closed:true}`.
A second close is NOT_AUTHORIZED, as is any foreign/invented identity. Closing a
guest never signs out its parent. The client does not wait for a close response
to dispose its local guest. A lost close response is safe; no new open is issued
until the old operation resolves or the slot generation has been retired.

Limits: three live identities/family/title, 24/account/title, 1024/title,
8192/process; 8 opens/account/title/rolling 60s, including idempotent opens. These
policy constants are approved, not XNA constants. Existing request/title/address
admission limits still apply. Check quotas atomically before RNG/SQL allocation;
prune expired families/identities first. Invalid requests do not allocate rows.
No guest credential is written to disk: the parent refresh credential remains
in the existing secure store; guest state is transient in CNA memory. Service
identity rows survive restart only while their family is still live, allowing
an existing running client to reconnect. A restarted CNA has no guest auto-login.

## Control operations and capability separation

Control envelope stays `{v:1,id,op,game,token,args}`, all strict shape/type/size,
TLS and title checks unchanged. Advertise `online-guests-v1` only when the schema,
directory, relay, cleanup, privacy and accounting paths all support it. New CNA
without that capability keeps existing account-only online calls; guest online
operations fail safely with the existing exception mapping, no synthetic account.

Add a distinct guest-aware directory family. The operation names are literal:
`sessions.createGuests`, `findGuests`, `joinGuests`, `joinInvitedGuests`,
`getGuests`, `touchGuests`, `updateGuests`, `leaveGuests`, `removeGuests`,
`addMembersGuests`, `relayTicketGuests`, `findQosGuests`, `qosHostTicketGuests`.
Each has the same arguments/result envelope as its un-suffixed operation except
for the participant encoding, roster fields and listing version below. The
shared `sessions.qosTicket {permit}` stays unchanged: permit captures the
originating search family/version. No guest uses it as the request actor.

Participants are a strict array of 1..4 objects. Each is **exactly one** of:

- account `{access:<64 lowerhex>}`;
- guest `{guest:<g:32lowerhex>,parentAccess:<64 lowerhex>}`.

Create/join/relay/qosHost arrays contain the authenticated machine owner account
and each represented guest's parent account. Order determines initial ordinals,
not authority. The top-level actor must be an account in the complete group.
A guest-only group, duplicate participant, mixed credential family, guest of a
parent on another machine, missing parent, wrong title or revoked family fails
atomically. Multiple real local accounts may each parent guests, within four
total participants/machine. AddMembersGuests contains only new participants
(1..3), and may refer to a parent already on the actor's machine; it must also
check the final group of <=4 and the existing machine's negotiated version.

Guest-aware snapshots add `guestProtocol:1` at the top level and add
`isGuest:<bool>,parentId:<accountId-or-null>` to **every** member. `userId` remains
the internal participant ID key: account ID for an account, `g:...` for a guest.
It is not permission to call account APIs with a guest ID. For account rows
parentId is null; for guest rows it must name a non-guest on the same machine.
HostId is always an account. Every ordinal is distinct 0..30 across both types.
Search advertisements add only guestProtocol (no member list/parent/family).
No response carries access/refresh credentials or hashes. Guest policy is read
from authenticated service state; peer hello flags never manufacture identity.

Sessions have an immutable guest-aware mode chosen by createGuests, including
when initially account-only. Legacy find/findQos filter out this mode **before**
pagination and permit issuance; legacy join/joinInvited/get/touch/add/relay/QoS
host operations reject it with NOT_SUPPORTED before any state change. Legacy
invites to this mode fail NOT_SUPPORTED when an old client accepts/joins; the
invitation never grants membership. New findGuests/findQosGuests return both
modes, with guestProtocol=1 for guest-aware and 0 for legacy advertisements.
New clients use legacy operations for mode0, and can join mode0 with accounts
only. A guest join/add to mode0 returns NOT_SUPPORTED. New getGuests etc require
mode1; there is no in-place mode upgrade that could strand old members. An old
client never silently treats a guest as an account or loses its IsGuest flag.
Old no-guest operations and result shapes remain byte/schema compatible.

Errors: malformed shape INVALID_ARGUMENT; missing/invalid top-level credential
UNAUTHENTICATED; foreign/invented/revoked/parent-mismatched guest NOT_AUTHORIZED
(no identity existence detail); wrong session mode NOT_SUPPORTED; duplicates or
parent elsewhere INVALID_STATE where the ordinary account membership contract
already uses it; capacity SESSION_FULL; four-local/global caps LIMIT_EXCEEDED;
open frequency RATE_LIMITED. Order syntax -> authenticate -> guest authority ->
session membership/mode -> state/capacity -> transaction; no partial mutation.
Existing End mappings remain: NOT_AUTHORIZED => GamerPrivilegeException,
NOT_SUPPORTED => NetworkNotAvailableException, full/not-joinable => typed
NetworkSessionJoinException. Do not throw raw JSON or bearer data.

Guest open/close are replayable only after the ordinary request owner/op checks,
with current family revalidation. An old successful open replay after close or
family revocation must fail NOT_AUTHORIZED, not return a resurrected identity.
Membership-changing operations follow existing durable request-outcome rules;
replays must revalidate live guests and cannot restore a closed identity.
Relay/QoS tickets retain GAP-17's non-durable one-use rule. Never persist raw
parentAccess in replay bodies/results/logs. New operation names must be added
to the service's operation classification, allowlists and error/diagnostic tests.

## Privileges, social policy and accounting

Owner-approved session admission: allow PlayerMatch and Ranked **lobby** guests under
the same four-local/31-total/private-slot arithmetic as accounts; PlayerMatch
playing joins require AllowJoinInProgress; Ranked playing joins/adds remain
refused. This is a CNA replacement-service policy pending native qualification,
not a claim that native Xbox Ranked admits guests. GetLocalGamers IL has no
managed guest restriction, but that does not settle native admission.

Online permission is the current parent's online_allowed AND live family AND
service capability, never an independent guest privilege. Guest purchase/trade/
premium grants are false. Communication/user-content/profile visibility may
only be as restrictive as the parent's current policy; account mute/block/avoid
and review/social routing evaluate the parent account on **all** participating
machines, including a secondary parent account, never the guest ID as a new
unblocked stranger. No guest independently uses friends/messages/party/avatar/
presence/invites APIs. Guest gameplay and bounded voice inherit the existing
machine route plus parent's current communication/block policy. Adding a guest
does not add a new microphone, invitation polling identity or event subscription.

Achievements: guest AwardAchievement refuses GamerPrivilegeException, no parent
award and no local fallback persistence. Guest leaderboard GetLeaderboard on
both SignedInGamer and NetworkGamer writers refuses GamerPrivilegeException
for service-backed guests in every session kind, including Playing and
LocalWithLeaderboards; no sink, parent attribution or independent row. Existing account writes remain unchanged. Guest IDs never enter account
leaderboard epochs, commit targets, arbitration candidate rows or TrueSkill
ownership. Ranked arbitration still has one report/vote per real machine, with
only real accounts as scored subjects. All-account machines have unchanged
majority rules. Every machine has an account, so no guest-only vote exists.
Final-write events for online guests are omitted; account events retain their
existing ordering/count. Raw server commit using a guest ID fails NOT_AUTHORIZED
atomically and preserves the account's prior rating. Guest identity-as-token
fails UNAUTHENTICATED for every account API. No new C/public XNA symbols needed.

## Lifetime and membership changes

Parent sign-out/family revoke/refresh replay/online privilege removal invalidates
all its guest identities and memberships in the same service transaction, bumps
roster revision once/session, invalidates related relay/QoS grants immediately.
If the parent is a machine owner, remove that whole machine as today, applying
migration-or-close before publishing a new snapshot. If it is a secondary account,
remove that account and its own guests while retaining the owner/other accounts.
Explicit guest close removes only that guest. Membership leases remain 90s,
relay-loss grace20s, client recovery15s as existing policy; no guest extends them.
Every authorization entry point prunes/revalidates before admitting; timer and
push hints accelerate cleanup but are not the security boundary.

A temporary relay loss retains all still-live guest IDs for the normal recovery
window. A lease expiry, leave, host kick, or terminal disconnect removes the
machine's complete group. Leaving a session alone does not dispose an otherwise
signed-in local guest identity; its parent can use it for a later session.
Host migration keeps each surviving guest bound to its **original local parent**,
not the new session host. Elect a surviving account-owned machine using existing
lowest-ordinal machine policy; host account stays its owner. Preserve IDs and
flags on surviving participants, obtain fresh full-group relay/QoS host grants,
and invalidate the old host epoch (GAP-17). Never promote a guest to host.

Client pending sign-in/open/admission jobs capture backend origin, parent
account/family authority, player slot generation and guest generation. They
must not retain mutable raw SignedInGamer pointers across async work. Sign-out
cancels local pending work synchronously; late responses cannot republish a
guest after parent-slot reuse, even for the same account. Dispatcher disposes
old gamers before SignedOut; callbacks only on update owner. Sign-out callback
exceptions must not interrupt disposal of siblings. No worker self-cycle or
blocking network call on destruction/game-frame getters.

## Connected protocol and storage

CNR relay v1 and CNQ v1 framing unchanged: authority is machine-bound. The new
relayTicketGuests must bind the exact complete typed group at issue, and validate
both account family and each guest-parent-family relation at redeem/revalidation.
Adding a member can retain old transport only after new group validation; closing
a guest forces an orderly ticket renewal, never an indefinite stale route. A
revoked grant drops packets immediately even if roster propagation is pending.
Guest-aware QoS host credentials validate the same typed group/current host;
permits use parent account budget, guest localCount consumes real capacity,
probes never create guest identities/memberships. CNQ nonce/rate/host-leg rules
remain GAP-17's. Old findQos cannot issue permits for mode1 sessions.

Connected control uses the existing trailing guest block: [0x47][u8 count]
[u8 flags]*count, bit0=guest. In mode1 write it when any guest exists; when none,
write no block, preserving old literal bytes. Mode1 service preflight must accept
only exact count matching the decoded roster/hello, flag bytes0/1, no repeated
block/unknown bits/trailing bytes. It must bound before decoding/allocation.
Mode0 preflight remains strict with no block. `ServiceRoster` validates all peer
IsGuest claims against authenticated rows and propagates guest bits in welcome,
join/add broadcasts and migration; never infer from a gamertag suffix.
ClientHello guest claims must also match service rows in the claimed order.

IMPORTANT implementation dependency: current ServiceGamePacketPolicy accepts
whole-machine join/leave broadcasts only, and ServiceENetSession disconnects a
machine whenever any of its identities changes. Mode1 needs revision-validated
participant deltas: an authoritative previous->next snapshot proves exactly the
added/departed IDs. Reject forged partial groups, but permit a legitimate single
guest close/secondary parent departure without disconnecting the owner. No peer
supplied partial list alone can authorize that delta. The control pump obtains
current service authority before processing a revision change. Buffer/drop stale
control within existing bounded recovery limits; never combine old IDs with new
flags. Tests must cover this seam as well as raw roster serialization.

Schema27 (approved): migration027 adds guest_identities, guest_open_times and
immutable guest-aware session mode. directory_members retains user_id as the
generic participant primary key (session_id,user_id). Generated virtual account_id
and guest_id columns select the users or guest_identities FK; parent_id and
family_id bind typed membership. UNIQUE(game_id,user_id) and
UNIQUE(session_id,ordinal) span both kinds. A deferred (session_id,parent_id)
self-FK cascades account removal; triggers validate same-machine, non-guest parent,
family/title/mode, including parent insertion after a guest in a transaction.
Existing account rows preserve IDs/ordinals/leases and have mode0/no family.
relay_ticket_members retains generic user_id, adding parent and guest references
and the existing family. Back up and verify before upgrading; rollback requires
restoring a pre-upgrade backup, not lowering user_version. Migration026 is unchanged.

All member/count/search/slot/ordinal/lease/remove/migration/relay/QoS/arbitration
SQL must operate on the typed participant relation, joining users only for
accounts and guest_identities for guest names; an inner JOIN users must not
silently erase guests. relay_ticket_members needs typed participant ID plus
parent family for guest records; revalidation compares exact live relations.
Migration rollback on injected failure leaves schema26 intact; foreign_key_check
and old account/restart/backups remain qualification gates. Persistent raw guest
IDs are public identifiers, not secrets; redact family IDs and access tokens in
diagnostics. Persistent replay results containing mode1 roster are non-authority.
