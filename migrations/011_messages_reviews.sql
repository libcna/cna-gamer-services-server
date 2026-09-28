-- SPDX-License-Identifier: MIT
-- Account-to-account text messages (Guide Messages/Compose) and player reviews (Guide
-- Player Review). Both are account-global social data, like the friends graph.
CREATE TABLE messages (
 id TEXT PRIMARY KEY, sender_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 recipient_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 text TEXT NOT NULL, created INTEGER NOT NULL, read INTEGER NOT NULL DEFAULT 0 CHECK(read IN (0,1))
);
CREATE INDEX messages_inbox ON messages(recipient_id,created,id);
CREATE INDEX messages_sender ON messages(sender_id,created);
CREATE TABLE player_reviews (
 reviewer_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 subject_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
 rating TEXT NOT NULL CHECK(rating IN ('prefer','avoid')), updated INTEGER NOT NULL,
 PRIMARY KEY(reviewer_id,subject_id)
);
PRAGMA user_version=11;
