// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Store.hpp"
#include <sqlite3.h>
#include <stdexcept>
// Test-only reconstruction of a genuine pre-27 schema; callers then set the old
// version they are testing. No new-schema tables/columns may survive that fixture.
inline constexpr const char* GuestSchemaDowngradeSQL=R"SQL(DROP TRIGGER guest_member_parent;
DROP TRIGGER account_member_parent;
CREATE TABLE saved_guest_members AS SELECT session_id,game_id,user_id,machine_id,private_slot,ordinal FROM directory_members;
DROP TABLE directory_members;
CREATE TABLE directory_members (
 session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 game_id TEXT NOT NULL REFERENCES titles(id), user_id TEXT NOT NULL REFERENCES users(id),
 machine_id TEXT NOT NULL REFERENCES directory_machines(id) ON DELETE CASCADE,
 private_slot INTEGER NOT NULL CHECK(private_slot IN (0,1)),
 ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 30),
 PRIMARY KEY(session_id,user_id), UNIQUE(session_id,ordinal), UNIQUE(game_id,user_id)
);
INSERT INTO directory_members SELECT * FROM saved_guest_members;
DROP TABLE saved_guest_members;
CREATE TABLE saved_guest_relay AS SELECT ticket_hash,user_id,family_id FROM relay_ticket_members;
DROP TABLE relay_ticket_members;
CREATE TABLE relay_ticket_members (
 ticket_hash TEXT NOT NULL REFERENCES relay_tickets(hash) ON DELETE CASCADE,
 user_id TEXT NOT NULL REFERENCES users(id),
 family_id TEXT NOT NULL REFERENCES refresh_families(id) ON DELETE CASCADE,
 PRIMARY KEY(ticket_hash,user_id)
);
INSERT INTO relay_ticket_members SELECT * FROM saved_guest_relay;
DROP TABLE saved_guest_relay;
DROP TABLE guest_open_times;
DROP TABLE guest_identities;
ALTER TABLE directory_sessions DROP COLUMN guest_protocol;
)SQL";
inline void removeGuestSchemaForMigration(CnaService::Store& store) {store.exec(GuestSchemaDowngradeSQL);}
inline void removeGuestSchemaForMigration(sqlite3* db) {
    if(sqlite3_exec(db,GuestSchemaDowngradeSQL,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("pre-27 fixture reconstruction");
}
