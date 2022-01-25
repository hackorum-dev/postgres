-- ok, superuser can create users with any set of privileges
CREATE ROLE regress_role_super SUPERUSER;
CREATE ROLE regress_role_bystander;
CREATE ROLE regress_role_admin CREATEDB CREATEROLE REPLICATION BYPASSRLS;
GRANT CREATE ON DATABASE regression TO regress_role_admin;

-- fail, only superusers can create users with these privileges
SET SESSION AUTHORIZATION regress_role_admin;
CREATE ROLE regress_nosuch_superuser SUPERUSER;
CREATE ROLE regress_nosuch_replication_bypassrls REPLICATION BYPASSRLS;
CREATE ROLE regress_nosuch_replication REPLICATION;
CREATE ROLE regress_nosuch_bypassrls BYPASSRLS;

-- fail, only superusers can own superusers
RESET SESSION AUTHORIZATION;
CREATE ROLE regress_nosuch_superuser AUTHORIZATION regress_role_admin SUPERUSER;

-- ok, superuser can create superusers belonging to other superusers
CREATE ROLE regress_superuser AUTHORIZATION regress_role_super SUPERUSER;

-- fail, can only create roles belonging to other roles that we belong to
SET SESSION AUTHORIZATION regress_role_admin;
CREATE ROLE regress_nosuch_alice AUTHORIZATION regress_role_super;
CREATE ROLE regress_nosuch_bob AUTHORIZATION regress_superuser;
CREATE ROLE regress_nosuch_charlie AUTHORIZATION regress_role_bystander;

-- ok, superuser can create users with these privileges for normal role
RESET SESSION AUTHORIZATION;
CREATE ROLE regress_replication_bypassrls AUTHORIZATION regress_role_admin REPLICATION BYPASSRLS;
CREATE ROLE regress_replication AUTHORIZATION regress_role_admin REPLICATION;
CREATE ROLE regress_bypassrls AUTHORIZATION regress_role_admin BYPASSRLS;

\du+ regress_superuser
\du+ regress_replication_bypassrls
\du+ regress_replication
\du+ regress_bypassrls

-- fail, roles are not allowed to own themselves
ALTER ROLE regress_bypassrls OWNER TO regress_bypassrls;

-- ok, having CREATEROLE is enough to create users with these privileges
SET SESSION AUTHORIZATION regress_role_admin;
CREATE ROLE regress_createdb CREATEDB;
CREATE ROLE regress_createrole CREATEROLE;
CREATE ROLE regress_login LOGIN;
CREATE ROLE regress_inherit INHERIT;
CREATE ROLE regress_connection_limit CONNECTION LIMIT 5;
CREATE ROLE regress_encrypted_password PASSWORD NULL;
CREATE ROLE regress_password_null
  CREATEDB CREATEROLE INHERIT CONNECTION LIMIT 2 ENCRYPTED PASSWORD 'foo'
  IN ROLE regress_createdb, regress_createrole;
COMMENT ON ROLE regress_password_null IS 'no login test role';

\du+ regress_createdb
\du+ regress_createrole
\du+ regress_login
\du+ regress_inherit
\du+ regress_connection_limit
\du+ regress_encrypted_password
\du+ regress_password_null

-- ok, backwards compatible noise words should be ignored
CREATE ROLE regress_noiseword SYSID 12345;

-- fail, cannot grant membership in superuser role
CREATE ROLE regress_nosuch_super IN ROLE regress_role_super;

-- fail, database owner cannot have members
CREATE ROLE regress_nosuch_dbowner IN ROLE pg_database_owner;

-- ok, can grant other users into a role
CREATE ROLE regress_inroles ROLE
	regress_role_super, regress_createdb, regress_createrole, regress_login,
	regress_inherit, regress_connection_limit, regress_encrypted_password, regress_password_null;

-- fail, cannot grant a role into itself
CREATE ROLE regress_nosuch_recursive ROLE regress_nosuch_recursive;

-- ok, can grant other users into a role with admin option
CREATE ROLE regress_adminroles ADMIN
	regress_role_super, regress_createdb, regress_createrole, regress_login,
	regress_inherit, regress_connection_limit, regress_encrypted_password, regress_password_null;

-- fail, cannot grant a role into itself with admin option
CREATE ROLE regress_nosuch_admin_recursive ADMIN regress_nosuch_admin_recursive;

-- fail, regress_createrole does not have CREATEDB privilege
SET SESSION AUTHORIZATION regress_createrole;
CREATE DATABASE regress_nosuch_db;

-- ok, regress_createrole can create new roles
CREATE ROLE regress_plainrole;

-- ok, roles with CREATEROLE can create new roles with it
CREATE ROLE regress_rolecreator CREATEROLE;

-- ok, roles with CREATEROLE can create new roles with privilege they lack
CREATE ROLE regress_tenant CREATEDB CREATEROLE LOGIN INHERIT CONNECTION LIMIT 5;

-- ok, regress_tenant can create objects within the database
SET SESSION AUTHORIZATION regress_tenant;
CREATE TABLE tenant_table (i integer);
CREATE INDEX tenant_idx ON tenant_table(i);
CREATE VIEW tenant_view AS SELECT * FROM pg_catalog.pg_class;
REVOKE ALL PRIVILEGES ON tenant_table FROM PUBLIC;

-- ok, owning role can manage owned role's objects
SET SESSION AUTHORIZATION regress_createrole;
DROP INDEX tenant_idx;
ALTER TABLE tenant_table ADD COLUMN t text;
DROP TABLE tenant_table;

-- fail, not a member of target role
ALTER VIEW tenant_view OWNER TO regress_role_admin;

-- ok
DROP VIEW tenant_view;

-- ok, can take ownership objects from owned roles
REASSIGN OWNED BY regress_tenant TO regress_createrole;

-- ok, having CREATEROLE is enough to create roles in privileged roles
CREATE ROLE regress_read_all_data IN ROLE pg_read_all_data;
CREATE ROLE regress_write_all_data IN ROLE pg_write_all_data;
CREATE ROLE regress_monitor IN ROLE pg_monitor;
CREATE ROLE regress_read_all_settings IN ROLE pg_read_all_settings;
CREATE ROLE regress_read_all_stats IN ROLE pg_read_all_stats;
CREATE ROLE regress_stat_scan_tables IN ROLE pg_stat_scan_tables;
CREATE ROLE regress_read_server_files IN ROLE pg_read_server_files;
CREATE ROLE regress_write_server_files IN ROLE pg_write_server_files;
CREATE ROLE regress_execute_server_program IN ROLE pg_execute_server_program;
CREATE ROLE regress_signal_backend IN ROLE pg_signal_backend;

-- fail, cannot create ownership cycles
RESET SESSION AUTHORIZATION;
REASSIGN OWNED BY regress_role_admin TO regress_tenant;
ALTER ROLE regress_role_admin OWNER TO regress_tenant;

-- ok, can take ownership from owned roles
SET SESSION AUTHORIZATION regress_role_admin;
ALTER ROLE regress_plainrole OWNER TO regress_role_admin;
REASSIGN OWNED BY regress_plainrole TO regress_role_admin;

-- ok, superuser roles can drop superuser roles they own
SET SESSION AUTHORIZATION regress_role_super;
DROP ROLE regress_superuser;

-- ok, non-superuser roles can drop non-superuser roles they own
SET SESSION AUTHORIZATION regress_role_admin;
DROP ROLE regress_replication_bypassrls;
DROP ROLE regress_replication;
DROP ROLE regress_bypassrls;
DROP ROLE regress_nosuch_super;
DROP ROLE regress_nosuch_dbowner;
DROP ROLE regress_nosuch_recursive;
DROP ROLE regress_nosuch_admin_recursive;
DROP ROLE regress_plainrole;

-- fail, cannot drop roles that own other roles
DROP ROLE regress_createrole;

-- ok, should be able to drop these non-superuser roles
DROP ROLE regress_createdb;
DROP ROLE regress_login;
DROP ROLE regress_inherit;
DROP ROLE regress_connection_limit;
DROP ROLE regress_encrypted_password;
DROP ROLE regress_password_null;
DROP ROLE regress_noiseword;
DROP ROLE regress_inroles;
DROP ROLE regress_adminroles;
DROP ROLE regress_rolecreator;
DROP ROLE regress_tenant;
DROP ROLE regress_read_all_data;
DROP ROLE regress_write_all_data;
DROP ROLE regress_monitor;
DROP ROLE regress_read_all_settings;
DROP ROLE regress_read_all_stats;
DROP ROLE regress_stat_scan_tables;
DROP ROLE regress_read_server_files;
DROP ROLE regress_write_server_files;
DROP ROLE regress_execute_server_program;
DROP ROLE regress_signal_backend;

-- fail, cannot drop ourself nor superusers
DROP ROLE regress_role_super;
DROP ROLE regress_role_admin;

-- ok, no more owned roles remain
DROP ROLE regress_createrole;

-- fail, cannot drop role with remaining privileges
RESET SESSION AUTHORIZATION;
DROP ROLE regress_role_admin;

-- ok, can drop role if we revoke privileges first
REVOKE CREATE ON DATABASE regression FROM regress_role_admin;
DROP ROLE regress_role_admin;
DROP ROLE regress_role_bystander;
DROP ROLE regress_role_super;
