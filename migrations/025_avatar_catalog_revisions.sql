-- SPDX-License-Identifier: MIT
-- Catalog v3 revision 2 (CNA description format 3): a catalog version may be replaced by a higher
-- revision that keeps every item it had; items of eleven more kinds (ears, earrings, wristwear,
-- gloves, rings, eyebrows, eye shadow, eyeliner, blush, lips: slots 7-16); and for every item the
-- lowest description format that can name it. Imported catalogs are revision 1 and their items
-- format 1, as they were.
ALTER TABLE avatar_catalogs ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK(revision BETWEEN 1 AND 65535);
CREATE TABLE avatar_catalog_items_next (
 version INTEGER NOT NULL REFERENCES avatar_catalogs(version) ON DELETE CASCADE,
 id INTEGER NOT NULL CHECK(id BETWEEN 1 AND 65535), slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 16),
 format INTEGER NOT NULL DEFAULT 1 CHECK(format BETWEEN 1 AND 3),
 PRIMARY KEY(version,id)
);
INSERT INTO avatar_catalog_items_next(version,id,slot) SELECT version,id,slot FROM avatar_catalog_items;
DROP TABLE avatar_catalog_items;
ALTER TABLE avatar_catalog_items_next RENAME TO avatar_catalog_items;
PRAGMA user_version=25;
