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
-- Test role pg_manage_vacuum_settings
-- Non-superuser with privileges to configure vacuum processes
CREATE ROLE regress_vacuum_admin NOSUPERUSER;
GRANT pg_manage_vacuum_settings TO regress_vacuum_admin;
-- Perform all operations as user 'regress_vacuum_admin' --
SET SESSION AUTHORIZATION regress_vacuum_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_vacuum_admin has insufficient privileges
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum;  -- fail, regress_vacuum_admin has insufficient privileges
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_vacuum_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_vacuum_admin has insufficient privileges
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_vacuum_admin has insufficient privileges
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_vacuum_admin has insufficient privileges
ALTER SYSTEM SET vacuum_failsafe_age = 50;  -- ok
ALTER SYSTEM RESET vacuum_failsafe_age;  -- ok
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing role pg_manage_vacuum_settings
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_vacuum_admin;
-- Test role pg_manage_autovacuum_settings
-- Non-superuser with privileges to configure autovacuum processes
CREATE ROLE regress_autovacuum_admin NOSUPERUSER;
GRANT pg_manage_autovacuum_settings TO regress_autovacuum_admin;
-- Perform all operations as user 'regress_autovacuum_admin' --
SET SESSION AUTHORIZATION regress_autovacuum_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_autovacuum_admin has insufficient privileges
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
ALTER SYSTEM SET jit_provider = 'llvmjit';  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM RESET jit_provider;  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- ok
ALTER SYSTEM RESET autovacuum;  -- ok
ALTER SYSTEM SET authentication_timeout = 50;  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM RESET authentication_timeout;  -- fail, regress_autovacuum_admin has insufficient privileges
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_autovacuum_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_autovacuum_admin has insufficient privileges
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_autovacuum_admin has insufficient privileges
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_autovacuum_admin has insufficient privileges
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing role pg_manage_autovacuum_settings
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_autovacuum_admin;
-- Test role pg_manage_logging_settings
-- Non-superuser with privileges to configure what and when to log
CREATE ROLE regress_log_whatwhen_admin NOSUPERUSER;
GRANT pg_manage_logging_settings TO regress_log_whatwhen_admin;
-- Perform all operations as user 'regress_log_whatwhen_admin' --
SET SESSION AUTHORIZATION regress_log_whatwhen_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_log_whatwhen_admin has insufficient privileges
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM SET trace_recovery_messages = 'log';  -- ok
ALTER SYSTEM RESET trace_recovery_messages;  -- ok
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_log_whatwhen_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_log_whatwhen_admin has insufficient privileges
SET backtrace_functions = 'partition_list_bsearch,partition_range_datum_bsearch,partition_hash_bsearch';  -- ok
RESET backtrace_functions;  -- ok
ALTER SYSTEM SET backtrace_functions = 'partition_list_bsearch,partition_range_datum_bsearch,partition_hash_bsearch';  -- ok
ALTER SYSTEM RESET backtrace_functions;  -- ok
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM SET log_connections = OFF;  -- ok
ALTER SYSTEM RESET log_connections;  -- ok
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_log_whatwhen_admin has insufficient privileges
ALTER SYSTEM SET escape_string_warning = OFF;  -- ok
ALTER SYSTEM RESET escape_string_warning;  -- ok
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing role pg_manage_logging_settings
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_log_whatwhen_admin;
-- Test combination of roles pg_manage_logging_settings and pg_write_server_files
-- Non-superuser with privileges to configure what, when and where to log
CREATE ROLE regress_log_full_admin NOSUPERUSER;
GRANT pg_manage_logging_settings TO regress_log_full_admin;
GRANT pg_write_server_files TO regress_log_full_admin;
-- Perform all operations as user 'regress_log_full_admin' --
SET SESSION AUTHORIZATION regress_log_full_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_log_full_admin has insufficient privileges
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
ALTER SYSTEM SET event_source = 'PostgreSQL';  -- ok
ALTER SYSTEM RESET event_source;  -- ok
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM SET trace_recovery_messages = 'log';  -- ok
ALTER SYSTEM RESET trace_recovery_messages;  -- ok
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_log_full_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_log_full_admin has insufficient privileges
SET backtrace_functions = 'partition_list_bsearch,partition_range_datum_bsearch,partition_hash_bsearch';  -- ok
RESET backtrace_functions;  -- ok
ALTER SYSTEM SET backtrace_functions = 'partition_list_bsearch,partition_range_datum_bsearch,partition_hash_bsearch';  -- ok
ALTER SYSTEM RESET backtrace_functions;  -- ok
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM SET log_connections = OFF;  -- ok
ALTER SYSTEM RESET log_connections;  -- ok
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_log_full_admin has insufficient privileges
ALTER SYSTEM SET escape_string_warning = OFF;  -- ok
ALTER SYSTEM RESET escape_string_warning;  -- ok
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing combination of roles pg_manage_logging_settings and pg_write_server_files
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_log_full_admin;
-- Test role pg_manage_replication_settings
-- Non-superuser with privileges to configure replication settings
CREATE ROLE regress_replication_admin NOSUPERUSER;
GRANT pg_manage_replication_settings TO regress_replication_admin;
-- Perform all operations as user 'regress_replication_admin' --
SET SESSION AUTHORIZATION regress_replication_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_replication_admin has insufficient privileges
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
ALTER SYSTEM SET max_replication_slots = 50;  -- ok
ALTER SYSTEM RESET max_replication_slots;  -- ok
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM SET synchronous_standby_names = 'fee, fi, fo, fum';  -- ok
ALTER SYSTEM RESET synchronous_standby_names;  -- ok
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_replication_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_replication_admin has insufficient privileges
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_replication_admin has insufficient privileges
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_replication_admin has insufficient privileges
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
ALTER SYSTEM SET wal_sender_timeout = 50;  -- ok
ALTER SYSTEM RESET wal_sender_timeout;  -- ok
-- Finished testing role pg_manage_replication_settings
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_replication_admin;
-- Test role pg_manage_connection_settings
-- Non-superuser with privileges to configure connections and authentication
CREATE ROLE regress_connection_admin NOSUPERUSER;
GRANT pg_manage_connection_settings TO regress_connection_admin;
-- Perform all operations as user 'regress_connection_admin' --
SET SESSION AUTHORIZATION regress_connection_admin;
-- PGC_BACKEND
SET ignore_system_indexes = OFF;  -- fail, cannot be set after connection start
RESET ignore_system_indexes;  -- fail, cannot be set after connection start
ALTER SYSTEM SET ignore_system_indexes = OFF;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM RESET ignore_system_indexes;  -- fail, regress_connection_admin has insufficient privileges
-- PGC_INTERNAL
SET block_size = 50;  -- fail, cannot be changed
RESET block_size;  -- fail, cannot be changed
ALTER SYSTEM SET block_size = 50;  -- fail, cannot be changed
ALTER SYSTEM RESET block_size;  -- fail, cannot be changed
-- PGC_POSTMASTER
SET autovacuum_freeze_max_age = 1000050000;  -- fail, requires restart
RESET autovacuum_freeze_max_age;  -- fail, requires restart
ALTER SYSTEM SET autovacuum_freeze_max_age = 1000050000;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum_freeze_max_age;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM SET bonjour = OFF;  -- ok
ALTER SYSTEM RESET bonjour;  -- ok
ALTER SYSTEM SET config_file = '/usr/local/data/postgresql.conf';  -- fail, cannot be changed
ALTER SYSTEM RESET config_file;  -- fail, cannot be changed
-- PGC_SIGHUP
SET autovacuum = OFF;  -- fail, requires reload
RESET autovacuum;  -- fail, requires reload
ALTER SYSTEM SET autovacuum = OFF;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM RESET autovacuum;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM SET authentication_timeout = 50;  -- ok
ALTER SYSTEM RESET authentication_timeout;  -- ok
-- PGC_SUSET
SET lc_messages = 'en_US.UTF-8';  -- fail, regress_connection_admin has insufficient privileges
RESET lc_messages;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM SET lc_messages = 'en_US.UTF-8';  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM RESET lc_messages;  -- fail, regress_connection_admin has insufficient privileges
-- PGC_SU_BACKEND
SET jit_debugging_support = OFF;  -- fail, cannot be set after connection start
RESET jit_debugging_support;  -- fail, cannot be set after connection start
ALTER SYSTEM SET jit_debugging_support = OFF;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM RESET jit_debugging_support;  -- fail, regress_connection_admin has insufficient privileges
-- PGC_USERSET
SET DateStyle = 'ISO, MDY';  -- ok
RESET DateStyle;  -- ok
ALTER SYSTEM SET DateStyle = 'ISO, MDY';  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM RESET DateStyle;  -- fail, regress_connection_admin has insufficient privileges
ALTER SYSTEM SET password_encryption = 'scram-sha-256';  -- ok
ALTER SYSTEM RESET password_encryption;  -- ok
ALTER SYSTEM SET ssl_renegotiation_limit = 0;  -- fail, cannot be changed
ALTER SYSTEM RESET ssl_renegotiation_limit;  -- fail, cannot be changed
-- Finished testing role pg_manage_connection_settings
RESET statement_timeout;
RESET SESSION AUTHORIZATION;
DROP ROLE regress_connection_admin;
