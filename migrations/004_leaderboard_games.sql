-- SPDX-License-Identifier: MS-PL
CREATE TABLE leaderboard_games(id TEXT PRIMARY KEY, game_id TEXT NOT NULL REFERENCES titles(id),
 owner_id TEXT NOT NULL REFERENCES users(id), kind TEXT NOT NULL, created INTEGER NOT NULL,
 expires INTEGER NOT NULL, committed TEXT NOT NULL DEFAULT '');
CREATE TABLE leaderboard_game_members(gameplay_id TEXT NOT NULL REFERENCES leaderboard_games(id) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id), PRIMARY KEY(gameplay_id,user_id));
PRAGMA user_version=4;
