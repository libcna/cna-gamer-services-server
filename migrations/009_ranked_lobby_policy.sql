-- SPDX-License-Identifier: MIT
UPDATE directory_sessions
SET allow_join=0,
    revision=CASE WHEN revision<2147483647 THEN revision+1 ELSE revision END
WHERE kind='ranked' AND allow_join<>0;
PRAGMA user_version=9;
