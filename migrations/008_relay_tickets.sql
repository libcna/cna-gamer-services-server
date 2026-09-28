-- SPDX-License-Identifier: MIT
CREATE TABLE relay_tickets (
 hash TEXT PRIMARY KEY, game_id TEXT NOT NULL REFERENCES titles(id),
 session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 machine_id TEXT NOT NULL REFERENCES directory_machines(id) ON DELETE CASCADE,
 owner_id TEXT NOT NULL REFERENCES users(id),
 participants_count INTEGER NOT NULL CHECK(participants_count BETWEEN 1 AND 4),
 created INTEGER NOT NULL, expires INTEGER NOT NULL, grant_expires INTEGER NOT NULL,
 used INTEGER NOT NULL DEFAULT 0 CHECK(used IN (0,1))
);
CREATE INDEX relay_tickets_machine ON relay_tickets(machine_id);
CREATE INDEX relay_tickets_title ON relay_tickets(game_id);
CREATE TABLE relay_ticket_members (
 ticket_hash TEXT NOT NULL REFERENCES relay_tickets(hash) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id),
 family_id TEXT NOT NULL REFERENCES refresh_families(id) ON DELETE CASCADE,
 PRIMARY KEY(ticket_hash,user_id)
);
PRAGMA user_version=8;
