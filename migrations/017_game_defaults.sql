-- SPDX-License-Identifier: MIT
-- An account's preferred game settings (XNA SignedInGamer.GameDefaults), as a CNA local profile
-- stores them: a JSON object of the known keys only.
ALTER TABLE users ADD COLUMN game_defaults TEXT NOT NULL DEFAULT '{}';
PRAGMA user_version=17;
