-- SPDX-License-Identifier: MIT
-- Parties (XNA SignedInGamer.PartySize, Guide.ShowParty/ShowPartySessions, SendPartyInvites): a
-- small group of friends across titles, one party per account; and join requests, the
-- invitations a player asks a friend's session for (never listed in anyone's inbox).
CREATE TABLE parties (
 id TEXT PRIMARY KEY, leader_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, created INTEGER NOT NULL
);
CREATE TABLE party_members (
 party_id TEXT NOT NULL REFERENCES parties(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL UNIQUE REFERENCES users(id) ON DELETE CASCADE, joined INTEGER NOT NULL,
 PRIMARY KEY(party_id,user_id)
);
CREATE TABLE party_invitations (
 party_id TEXT NOT NULL REFERENCES parties(id) ON DELETE CASCADE,
 sender_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 recipient_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE, created INTEGER NOT NULL,
 PRIMARY KEY(party_id,recipient_id)
);
CREATE INDEX party_invitation_inbox ON party_invitations(recipient_id,created);
ALTER TABLE session_invitations ADD COLUMN requested INTEGER NOT NULL DEFAULT 0 CHECK(requested IN (0,1));
PRAGMA user_version=20;
