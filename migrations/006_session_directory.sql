-- SPDX-License-Identifier: MIT
CREATE TABLE directory_sessions (
 id TEXT PRIMARY KEY, game_id TEXT NOT NULL REFERENCES titles(id),
 host_id TEXT NOT NULL REFERENCES users(id), host_machine TEXT NOT NULL,
 kind TEXT NOT NULL CHECK(kind IN ('player','ranked')),
 state TEXT NOT NULL DEFAULT 'lobby' CHECK(state IN ('lobby','playing')),
 max_gamers INTEGER NOT NULL CHECK(max_gamers BETWEEN 2 AND 31),
 private_slots INTEGER NOT NULL CHECK(private_slots BETWEEN 0 AND max_gamers),
 properties TEXT NOT NULL, allow_join INTEGER NOT NULL DEFAULT 0 CHECK(allow_join IN (0,1)),
 revision INTEGER NOT NULL DEFAULT 1 CHECK(revision BETWEEN 1 AND 2147483647),
 created INTEGER NOT NULL, expires INTEGER NOT NULL
);
CREATE INDEX directory_title_search ON directory_sessions(game_id,kind,created,id);
CREATE TABLE directory_machines (
 id TEXT PRIMARY KEY, session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 owner_id TEXT NOT NULL REFERENCES users(id), expires INTEGER NOT NULL,
 UNIQUE(session_id,owner_id)
);
CREATE INDEX directory_machine_session ON directory_machines(session_id);
CREATE TABLE directory_members (
 session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 game_id TEXT NOT NULL REFERENCES titles(id), user_id TEXT NOT NULL REFERENCES users(id),
 machine_id TEXT NOT NULL REFERENCES directory_machines(id) ON DELETE CASCADE,
 private_slot INTEGER NOT NULL CHECK(private_slot IN (0,1)),
 ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 30),
 PRIMARY KEY(session_id,user_id), UNIQUE(session_id,ordinal), UNIQUE(game_id,user_id)
);
PRAGMA user_version=6;
