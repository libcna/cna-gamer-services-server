-- SPDX-License-Identifier: MIT
-- XNA FriendGamer.HasVoice: whether the client behind a live sign-in can currently talk (voice
-- built and enabled, a capture device present). Reported with the heartbeat; seen by friends only
-- while that sign-in is online.
ALTER TABLE sessions ADD COLUMN voice INTEGER NOT NULL DEFAULT 0 CHECK(voice IN (0,1));
PRAGMA user_version=23;
