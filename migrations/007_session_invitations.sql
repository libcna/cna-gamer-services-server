-- SPDX-License-Identifier: MIT
CREATE TABLE session_invitations (
 id TEXT PRIMARY KEY, game_id TEXT NOT NULL REFERENCES titles(id),
 session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 sender_id TEXT NOT NULL REFERENCES users(id), recipient_id TEXT NOT NULL REFERENCES users(id),
 status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','accepted','dismissed','used')),
 created INTEGER NOT NULL, expires INTEGER NOT NULL, accepted_at INTEGER NOT NULL DEFAULT 0,
 used_machine TEXT NOT NULL DEFAULT ''
);
CREATE INDEX invite_inbox ON session_invitations(game_id,recipient_id,created,id);
CREATE INDEX invite_sender_quota ON session_invitations(game_id,sender_id,created);
CREATE TABLE invitation_send_limits (
 game_id TEXT NOT NULL REFERENCES titles(id), user_id TEXT NOT NULL REFERENCES users(id),
 window_start INTEGER NOT NULL, created_count INTEGER NOT NULL CHECK(created_count BETWEEN 1 AND 32),
 PRIMARY KEY(game_id,user_id)
);
PRAGMA user_version=7;
