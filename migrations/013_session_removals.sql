-- SPDX-License-Identifier: MIT
-- A host removing a machine from its session (XNA NetworkMachine.RemoveFromSession). The removed
-- users are told why for as long as the session lasts, instead of only finding membership gone.
CREATE TABLE directory_removals (
 session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 PRIMARY KEY(session_id,user_id)
);
PRAGMA user_version=13;
