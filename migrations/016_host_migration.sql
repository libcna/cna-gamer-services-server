-- SPDX-License-Identifier: MIT
-- XNA NetworkSession.AllowHostMigration for directory sessions: when set and the host machine
-- leaves or its lease lapses, the service hands the session to another machine instead of ending it.
ALTER TABLE directory_sessions ADD COLUMN allow_migration INTEGER NOT NULL DEFAULT 0 CHECK(allow_migration IN (0,1));
PRAGMA user_version=16;
