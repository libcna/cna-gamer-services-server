-- SPDX-License-Identifier: MIT
CREATE TABLE refresh_families(id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id),
 game_id TEXT NOT NULL REFERENCES titles(id), expires INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0,
 rotations INTEGER NOT NULL DEFAULT 0);
CREATE INDEX refresh_families_user ON refresh_families(user_id);
CREATE TABLE refresh_credentials(hash TEXT PRIMARY KEY, family_id TEXT NOT NULL REFERENCES refresh_families(id) ON DELETE CASCADE,
 used INTEGER NOT NULL DEFAULT 0);
ALTER TABLE sessions ADD COLUMN refresh_family TEXT REFERENCES refresh_families(id) ON DELETE CASCADE;
CREATE INDEX sessions_refresh_family ON sessions(refresh_family);
PRAGMA user_version=5;
