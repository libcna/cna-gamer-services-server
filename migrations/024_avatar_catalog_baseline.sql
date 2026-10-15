-- SPDX-License-Identifier: MIT
-- Avatar catalog v3 is the baseline. Catalogs 1 and 2 were CNA development catalogs that no
-- supported client draws: an avatar naming one (catalog version in description bytes 8-9, little
-- endian) is removed, and the account keeps everything else and has no avatar until it sets one.
-- Then those catalogs' items, file links and manifests go. Their content-addressed files stay in
-- `assets`, where other catalogs, titles and pictures may share a hash; a file no catalog lists is
-- not served as a catalog file. From here on fresh and upgraded databases refuse a catalog, or an
-- avatar, below the baseline.
DELETE FROM avatars WHERE hex(substr(description,9,2)) IN ('0000','0100','0200');
DELETE FROM avatar_catalog_items WHERE version<3;
DELETE FROM avatar_catalog_assets WHERE version<3;
DELETE FROM avatar_catalogs WHERE version<3;
CREATE TRIGGER IF NOT EXISTS avatar_catalogs_baseline_insert BEFORE INSERT ON avatar_catalogs
 WHEN NEW.version<3 BEGIN SELECT RAISE(ABORT,'avatar catalog below the baseline'); END;
CREATE TRIGGER IF NOT EXISTS avatar_catalogs_baseline_update BEFORE UPDATE OF version ON avatar_catalogs
 WHEN NEW.version<3 BEGIN SELECT RAISE(ABORT,'avatar catalog below the baseline'); END;
CREATE TRIGGER IF NOT EXISTS avatars_baseline_insert BEFORE INSERT ON avatars
 WHEN hex(substr(NEW.description,9,2)) IN ('0000','0100','0200')
 BEGIN SELECT RAISE(ABORT,'avatar on a catalog below the baseline'); END;
CREATE TRIGGER IF NOT EXISTS avatars_baseline_update BEFORE UPDATE OF description ON avatars
 WHEN hex(substr(NEW.description,9,2)) IN ('0000','0100','0200')
 BEGIN SELECT RAISE(ABORT,'avatar on a catalog below the baseline'); END;
PRAGMA user_version=24;
