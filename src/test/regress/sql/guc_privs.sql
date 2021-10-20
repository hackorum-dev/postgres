-- Test superuser
-- Superuser DBA
CREATE ROLE regress_admin SUPERUSER;
-- Perform all operations as user 'regress_admin' --
SET SESSION AUTHORIZATION regress_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- ok
ALTER SYSTEM RESET ignore_system_indexes;  -- ok
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- ok
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- ok
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- ok
ALTER SYSTEM RESET autovacuum;  -- ok
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- ok
RESET lc_messages;  -- ok
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- ok
ALTER SYSTEM RESET lc_messages;  -- ok
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- ok
ALTER SYSTEM RESET jit_debugging_support;  -- ok
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- ok
ALTER SYSTEM RESET DateStyle;  -- ok
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing superuser
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_admin;
-- Test role pg_manage_host_resource_settings
-- Non-superuser with privileges to configure host resource usage
CREATE ROLE regress_host_resource_admin NOSUPERUSER;
GRANT pg_manage_host_resource_settings TO regress_host_resource_admin;
-- Perform all operations as user 'regress_host_resource_admin' --
SET SESSION AUTHORIZATION regress_host_resource_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_host_resource_admin has insufficient privileges
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
ALTER SYSTEM SET max_locks_per_transaction = 50;  -- ok
ALTER SYSTEM RESET max_locks_per_transaction;  -- ok
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM SET max_pred_locks_per_page = 50;  -- ok
ALTER SYSTEM RESET max_pred_locks_per_page;  -- ok
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_host_resource_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_host_resource_admin has insufficient privileges
SET temp_file_limit = 50;  -- ok
RESET temp_file_limit;  -- ok
ALTER SYSTEM SET temp_file_limit = 50;  -- ok
ALTER SYSTEM RESET temp_file_limit;  -- ok
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_host_resource_admin has insufficient privileges
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_host_resource_admin has insufficient privileges
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
ALTER SYSTEM SET backend_flush_after = 128;  -- ok
ALTER SYSTEM RESET backend_flush_after;  -- ok
-- Finished testing role pg_manage_host_resource_settings
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_host_resource_admin;
