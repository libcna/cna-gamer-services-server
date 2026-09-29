-- SPDX-License-Identifier: MIT
-- The oldest game version a title still accepts (XNA GameUpdateRequiredException): requests from an
-- older version, or one that states none, are refused with UPDATE_REQUIRED. Empty accepts every version.
ALTER TABLE titles ADD COLUMN minimum_version TEXT NOT NULL DEFAULT '';
PRAGMA user_version=19;
