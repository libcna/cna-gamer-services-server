-- SPDX-License-Identifier: MIT
-- A recorded request ID commits in the same transaction as the change it made, with the account
-- and operation that used it and the outcome, so a repeat after a lost response is answered from
-- the record instead of being refused or executed twice. Rows written before this migration have
-- no operation and keep answering DUPLICATE_REQUEST.
ALTER TABLE request_ids ADD COLUMN user_id TEXT NOT NULL DEFAULT '';
ALTER TABLE request_ids ADD COLUMN op TEXT NOT NULL DEFAULT '';
ALTER TABLE request_ids ADD COLUMN outcome TEXT NOT NULL DEFAULT '';
ALTER TABLE request_ids ADD COLUMN result TEXT;
PRAGMA user_version=21;
