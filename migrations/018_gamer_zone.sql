-- SPDX-License-Identifier: MIT
-- The style of social gaming a member prefers (XNA GamerProfile.GamerZone), chosen by the member.
ALTER TABLE users ADD COLUMN gamer_zone TEXT NOT NULL DEFAULT 'unknown' CHECK(gamer_zone IN ('unknown','recreation','pro','family','underground'));
PRAGMA user_version=18;
