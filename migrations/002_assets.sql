-- SPDX-License-Identifier: MIT
CREATE TABLE assets(hash TEXT PRIMARY KEY CHECK(length(hash)=64), mime TEXT NOT NULL,
 size INTEGER NOT NULL CHECK(size BETWEEN 1 AND 16777216), bytes BLOB NOT NULL);
CREATE TABLE title_assets(game_id TEXT NOT NULL REFERENCES titles(id), hash TEXT NOT NULL REFERENCES assets(hash),
 PRIMARY KEY(game_id,hash));
ALTER TABLE users ADD COLUMN picture TEXT NOT NULL DEFAULT '';
PRAGMA user_version=2;
