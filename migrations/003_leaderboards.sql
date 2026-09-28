-- SPDX-License-Identifier: MIT
CREATE TABLE leaderboards(game_id TEXT NOT NULL REFERENCES titles(id), key TEXT NOT NULL,
 mode INTEGER NOT NULL, ascending INTEGER NOT NULL, aggregation TEXT NOT NULL,
 arbitrated INTEGER NOT NULL, columns TEXT NOT NULL, PRIMARY KEY(game_id,key,mode));
CREATE TABLE leaderboard_entries(game_id TEXT NOT NULL, key TEXT NOT NULL, mode INTEGER NOT NULL,
 user_id TEXT NOT NULL REFERENCES users(id), rating INTEGER NOT NULL, columns TEXT NOT NULL,
 updated INTEGER NOT NULL, PRIMARY KEY(game_id,key,mode,user_id),
 FOREIGN KEY(game_id,key,mode) REFERENCES leaderboards(game_id,key,mode));
PRAGMA user_version=3;
