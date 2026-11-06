-- SPDX-License-Identifier: MIT
-- GAP-16 CNA replacement-service policy. Back up the database before upgrading.
ALTER TABLE directory_sessions ADD COLUMN guest_protocol INTEGER NOT NULL DEFAULT 0 CHECK(guest_protocol IN (0,1));
CREATE TABLE guest_identities (
 id TEXT PRIMARY KEY CHECK(length(id)=34 AND substr(id,1,2)='g:'),
 parent_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 family_id TEXT NOT NULL REFERENCES refresh_families(id) ON DELETE CASCADE,
 game_id TEXT NOT NULL REFERENCES titles(id),
 slot INTEGER NOT NULL CHECK(slot BETWEEN 1 AND 3),
 gamertag TEXT NOT NULL CHECK(length(gamertag) BETWEEN 5 AND 32),
 UNIQUE(family_id,game_id,slot)
);
CREATE INDEX guest_parent ON guest_identities(parent_id,game_id);
CREATE TABLE guest_open_times (
 parent_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 game_id TEXT NOT NULL REFERENCES titles(id), at INTEGER NOT NULL
);
CREATE INDEX guest_open_budget ON guest_open_times(parent_id,game_id,at);
-- Keep user_id as the participant key for existing directory SQL. Generated typed
-- foreign keys ensure a guest is never an account, while one ordinal space covers both.
CREATE TABLE directory_members_new (
 session_id TEXT NOT NULL REFERENCES directory_sessions(id) ON DELETE CASCADE,
 game_id TEXT NOT NULL REFERENCES titles(id), user_id TEXT NOT NULL,
 machine_id TEXT NOT NULL REFERENCES directory_machines(id) ON DELETE CASCADE,
 private_slot INTEGER NOT NULL CHECK(private_slot IN (0,1)),
 ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 30),
 account_id TEXT GENERATED ALWAYS AS (CASE WHEN substr(user_id,1,2)!='g:' THEN user_id END) VIRTUAL REFERENCES users(id) ON DELETE CASCADE,
 guest_id TEXT GENERATED ALWAYS AS (CASE WHEN substr(user_id,1,2)='g:' THEN user_id END) VIRTUAL REFERENCES guest_identities(id) ON DELETE CASCADE,
 parent_id TEXT REFERENCES users(id) ON DELETE CASCADE,
 family_id TEXT REFERENCES refresh_families(id) ON DELETE CASCADE,
 CHECK((guest_id IS NULL AND parent_id IS NULL) OR (guest_id IS NOT NULL AND parent_id IS NOT NULL AND family_id IS NOT NULL)),
 PRIMARY KEY(session_id,user_id), UNIQUE(session_id,ordinal), UNIQUE(game_id,user_id),
 FOREIGN KEY(session_id,parent_id) REFERENCES directory_members_new(session_id,user_id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED
);
INSERT INTO directory_members_new(session_id,game_id,user_id,machine_id,private_slot,ordinal)
 SELECT session_id,game_id,user_id,machine_id,private_slot,ordinal FROM directory_members;
DROP TABLE directory_members;
ALTER TABLE directory_members_new RENAME TO directory_members;
CREATE TRIGGER guest_member_parent BEFORE INSERT ON directory_members WHEN NEW.guest_id IS NOT NULL BEGIN
 SELECT CASE WHEN NOT EXISTS(SELECT 1 FROM guest_identities g JOIN directory_sessions d ON d.id=NEW.session_id
  WHERE g.id=NEW.guest_id AND g.parent_id=NEW.parent_id AND g.family_id=NEW.family_id AND g.game_id=NEW.game_id AND d.guest_protocol=1)
  THEN RAISE(ABORT,'invalid guest authority') END;
 SELECT CASE WHEN EXISTS(SELECT 1 FROM directory_members p WHERE p.session_id=NEW.session_id AND p.user_id=NEW.parent_id
  AND (p.machine_id!=NEW.machine_id OR p.guest_id IS NOT NULL OR p.family_id!=NEW.family_id))
  THEN RAISE(ABORT,'invalid guest parent') END;
END;
CREATE TRIGGER account_member_parent AFTER INSERT ON directory_members WHEN NEW.account_id IS NOT NULL BEGIN
 SELECT CASE WHEN EXISTS(SELECT 1 FROM directory_members g WHERE g.session_id=NEW.session_id AND g.parent_id=NEW.user_id
  AND (g.machine_id!=NEW.machine_id OR g.family_id!=NEW.family_id)) THEN RAISE(ABORT,'invalid guest parent') END;
END;
CREATE TABLE relay_ticket_members_new (
 ticket_hash TEXT NOT NULL REFERENCES relay_tickets(hash) ON DELETE CASCADE,
 user_id TEXT NOT NULL, parent_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 guest_id TEXT GENERATED ALWAYS AS (CASE WHEN substr(user_id,1,2)='g:' THEN user_id END) VIRTUAL REFERENCES guest_identities(id) ON DELETE CASCADE,
 family_id TEXT NOT NULL REFERENCES refresh_families(id) ON DELETE CASCADE,
 PRIMARY KEY(ticket_hash,user_id)
);
INSERT INTO relay_ticket_members_new(ticket_hash,user_id,parent_id,family_id) SELECT ticket_hash,user_id,user_id,family_id FROM relay_ticket_members;
DROP TABLE relay_ticket_members;
ALTER TABLE relay_ticket_members_new RENAME TO relay_ticket_members;
PRAGMA user_version=27;
