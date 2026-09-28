-- SPDX-License-Identifier: MIT
-- Avatars: one CNA-encoded 1021-byte description per account, and imported avatar asset catalogs
-- whose files live in the content-addressed asset store. Catalogs only grow: a newer version keeps
-- every item id of the previous one in the same slot.
CREATE TABLE avatars (
 user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
 description BLOB NOT NULL CHECK(length(description)=1021),
 revision INTEGER NOT NULL CHECK(revision>=1), updated INTEGER NOT NULL
);
CREATE TABLE avatar_catalogs (
 version INTEGER PRIMARY KEY CHECK(version BETWEEN 1 AND 65535),
 manifest TEXT NOT NULL, imported INTEGER NOT NULL
);
CREATE TABLE avatar_catalog_items (
 version INTEGER NOT NULL REFERENCES avatar_catalogs(version) ON DELETE CASCADE,
 id INTEGER NOT NULL CHECK(id BETWEEN 1 AND 65535), slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 5),
 PRIMARY KEY(version,id)
);
CREATE TABLE avatar_catalog_assets (
 version INTEGER NOT NULL REFERENCES avatar_catalogs(version) ON DELETE CASCADE,
 name TEXT NOT NULL, hash TEXT NOT NULL REFERENCES assets(hash), PRIMARY KEY(version,name)
);
CREATE INDEX avatar_catalog_asset_hash ON avatar_catalog_assets(hash);
PRAGMA user_version=12;
