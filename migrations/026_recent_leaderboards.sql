-- SPDX-License-Identifier: MIT
-- CNA GAP-18 policy activation. Existing updated timestamps are preserved.
UPDATE leaderboards SET aggregation='best',ascending=(key='BestTimeRecent')
 WHERE key IN ('BestScoreRecent','BestTimeRecent');
CREATE INDEX leaderboard_recent_expiry ON leaderboard_entries(game_id,key,mode,updated)
 WHERE key IN ('BestScoreRecent','BestTimeRecent');
PRAGMA user_version=26;
