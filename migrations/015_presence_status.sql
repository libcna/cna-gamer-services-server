-- SPDX-License-Identifier: MIT
-- An account's online status as its friends see it (XNA FriendGamer.IsAway/IsBusy): account-wide,
-- set by the player, never inferred.
ALTER TABLE users ADD COLUMN status TEXT NOT NULL DEFAULT 'online' CHECK(status IN ('online','away','busy'));
PRAGMA user_version=15;
