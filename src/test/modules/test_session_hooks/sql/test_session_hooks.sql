--
-- Tests for start session hook
--

-- Only activity from role regress_sess_hook_usr2 is logged.
ALTER ROLE regress_sess_hook_usr1 SUPERUSER LOGIN;
ALTER ROLE regress_sess_hook_usr2 SUPERUSER LOGIN;
\set prevdb :DBNAME
\set prevusr :USER
CREATE TABLE session_hook_log(id SERIAL, dbname TEXT, username TEXT, hook_at TEXT);
SELECT * FROM session_hook_log ORDER BY id;
\c :prevdb regress_sess_hook_usr1
SELECT * FROM session_hook_log ORDER BY id;
\c :prevdb regress_sess_hook_usr2
SELECT * FROM session_hook_log ORDER BY id;
\c :prevdb :prevusr
SELECT * FROM session_hook_log ORDER BY id;
