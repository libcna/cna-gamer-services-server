-- SPDX-License-Identifier: MIT
-- XNA GamerPrivileges as account policy the operator sets (CNA's stand-in for parental controls),
-- and each member's own block list.
ALTER TABLE users ADD COLUMN privilege_communication TEXT NOT NULL DEFAULT 'everyone' CHECK(privilege_communication IN ('everyone','friends','blocked'));
ALTER TABLE users ADD COLUMN privilege_profile_viewing TEXT NOT NULL DEFAULT 'everyone' CHECK(privilege_profile_viewing IN ('everyone','friends','blocked'));
ALTER TABLE users ADD COLUMN privilege_user_content TEXT NOT NULL DEFAULT 'everyone' CHECK(privilege_user_content IN ('everyone','friends','blocked'));
ALTER TABLE users ADD COLUMN privilege_trade INTEGER NOT NULL DEFAULT 1 CHECK(privilege_trade IN (0,1));
ALTER TABLE users ADD COLUMN privilege_purchase INTEGER NOT NULL DEFAULT 1 CHECK(privilege_purchase IN (0,1));
ALTER TABLE users ADD COLUMN privilege_premium INTEGER NOT NULL DEFAULT 1 CHECK(privilege_premium IN (0,1));
CREATE TABLE blocks(user_id TEXT NOT NULL REFERENCES users(id), blocked_id TEXT NOT NULL REFERENCES users(id),
 created INTEGER NOT NULL, PRIMARY KEY(user_id,blocked_id), CHECK(user_id!=blocked_id));
CREATE INDEX blocks_blocked ON blocks(blocked_id,user_id);
PRAGMA user_version=22;
