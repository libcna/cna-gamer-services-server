-- SPDX-License-Identifier: MIT
-- A Ranked gameplay round opened when the host moves Lobby->Playing. Rounds deliberately have
-- no foreign key to the directory: a host may close the session before every machine reports.
CREATE TABLE arbitration_rounds (
 id TEXT PRIMARY KEY, game_id TEXT NOT NULL REFERENCES titles(id) ON DELETE CASCADE,
 session_id TEXT NOT NULL, start_revision INTEGER NOT NULL, end_revision INTEGER,
 members TEXT NOT NULL, machines TEXT NOT NULL,
 created INTEGER NOT NULL, updated INTEGER NOT NULL, resolved INTEGER NOT NULL DEFAULT 0 CHECK(resolved IN (0,1))
);
CREATE INDEX arbitration_round_session ON arbitration_rounds(session_id,start_revision);
CREATE INDEX arbitration_round_pending ON arbitration_rounds(game_id,resolved,updated);
CREATE TABLE arbitration_submissions (
 round_id TEXT NOT NULL REFERENCES arbitration_rounds(id) ON DELETE CASCADE,
 machine_id TEXT NOT NULL, entries TEXT NOT NULL, digest TEXT NOT NULL,
 PRIMARY KEY(round_id,machine_id)
);
PRAGMA user_version=10;
