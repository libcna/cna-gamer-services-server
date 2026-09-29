-- SPDX-License-Identifier: MIT
-- Avatar catalogs may list feature items (facial hair) a format 2 description names: slot 6.
CREATE TABLE avatar_catalog_items_next (
 version INTEGER NOT NULL REFERENCES avatar_catalogs(version) ON DELETE CASCADE,
 id INTEGER NOT NULL CHECK(id BETWEEN 1 AND 65535), slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 6),
 PRIMARY KEY(version,id)
);
INSERT INTO avatar_catalog_items_next(version,id,slot) SELECT version,id,slot FROM avatar_catalog_items;
DROP TABLE avatar_catalog_items;
ALTER TABLE avatar_catalog_items_next RENAME TO avatar_catalog_items;
PRAGMA user_version=14;
