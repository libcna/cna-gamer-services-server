-- SPDX-License-Identifier: MS-PL
CREATE TABLE titles(id TEXT PRIMARY KEY, name TEXT NOT NULL);
CREATE TABLE users(id TEXT PRIMARY KEY, username TEXT UNIQUE COLLATE NOCASE NOT NULL,
 gamertag TEXT UNIQUE COLLATE NOCASE NOT NULL, salt TEXT NOT NULL, verifier TEXT NOT NULL,
 motto TEXT NOT NULL DEFAULT '', region TEXT NOT NULL DEFAULT 'US', online_allowed INTEGER NOT NULL DEFAULT 1);
CREATE TABLE sessions(hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id),
 game_id TEXT NOT NULL REFERENCES titles(id), expires INTEGER NOT NULL, last_seen INTEGER NOT NULL);
CREATE TABLE request_ids(game_id TEXT NOT NULL REFERENCES titles(id), id TEXT NOT NULL, created INTEGER NOT NULL,
 PRIMARY KEY(game_id,id));
CREATE TABLE achievements(game_id TEXT NOT NULL REFERENCES titles(id), key TEXT NOT NULL, name TEXT NOT NULL,
 description TEXT NOT NULL, how_to_earn TEXT NOT NULL, score INTEGER NOT NULL CHECK(score BETWEEN 0 AND 1000),
 display INTEGER NOT NULL, picture TEXT NOT NULL DEFAULT '', PRIMARY KEY(game_id,key));
CREATE TABLE earned(user_id TEXT NOT NULL REFERENCES users(id), game_id TEXT NOT NULL, key TEXT NOT NULL,
 ticks INTEGER NOT NULL, PRIMARY KEY(user_id,game_id,key),
 FOREIGN KEY(game_id,key) REFERENCES achievements(game_id,key));
CREATE TABLE friends(user_id TEXT NOT NULL REFERENCES users(id), friend_id TEXT NOT NULL REFERENCES users(id),
 PRIMARY KEY(user_id,friend_id), CHECK(user_id != friend_id));
CREATE TABLE presence(user_id TEXT NOT NULL REFERENCES users(id), game_id TEXT NOT NULL REFERENCES titles(id),
 mode INTEGER NOT NULL DEFAULT 0, text TEXT NOT NULL DEFAULT '', PRIMARY KEY(user_id,game_id));
PRAGMA user_version=1;
