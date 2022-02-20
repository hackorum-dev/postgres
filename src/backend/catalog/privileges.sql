/*
 * Set up privileges
 *
 * We mark most system catalogs as world-readable.  We don't currently have
 * to touch functions, languages, or databases, because their default
 * permissions are OK.
 *
 * Some objects may require different permissions by default, so we
 * make sure we don't overwrite privilege sets that have already been
 * set (NOT NULL).
 *
 * Also populate pg_init_privs to save what the privileges are at init
 * time.  This is used by pg_dump to allow users to change privileges
 * on catalog objects and to have those privilege changes preserved
 * across dump/reload and pg_upgrade.
 *
 * Note that pg_init_privs is only for per-database objects and therefore
 * we don't include databases or tablespaces.
 */

UPDATE pg_class
  SET relacl = (SELECT array_agg(a.acl) FROM
 (SELECT '=r/"POSTGRES"' as acl
  UNION SELECT unnest(pg_catalog.acldefault(
    CASE WHEN relkind = 'S' THEN 's'
         ELSE 'r' END::"char", 10::oid)) -- FIXME, inlined BOOTSTRAP_SUPERUSERID
 ) as a)
  WHERE relkind IN ('r', 'v', 'm','S')
  AND relacl IS NULL;

GRANT USAGE ON SCHEMA pg_catalog, public TO PUBLIC;
REVOKE ALL ON pg_largeobject FROM PUBLIC;
INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE relname = 'pg_class'),
        0,
        relacl,
        'i'
    FROM
        pg_class
    WHERE
        relacl IS NOT NULL
        AND relkind IN ('r', 'v', 'm','S');

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        pg_class.oid,
        (SELECT oid FROM pg_class WHERE relname = 'pg_class'),
        pg_attribute.attnum,
        pg_attribute.attacl,
        'i'
    FROM
        pg_class
        JOIN pg_attribute ON (pg_class.oid = pg_attribute.attrelid)
    WHERE
        pg_attribute.attacl IS NOT NULL
        AND pg_class.relkind IN ('r', 'v', 'm', 'S');

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),
        0,
        proacl,
        'i'
    FROM
        pg_proc
    WHERE
        proacl IS NOT NULL;

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE relname = 'pg_type'),
        0,
        typacl,
        'i'
    FROM
        pg_type
    WHERE
        typacl IS NOT NULL;

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE relname = 'pg_language'),
        0,
        lanacl,
        'i'
    FROM
        pg_language
    WHERE
        lanacl IS NOT NULL;

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE
         relname = 'pg_largeobject_metadata'),
        0,
        lomacl,
        'i'
    FROM
        pg_largeobject_metadata
    WHERE
        lomacl IS NOT NULL;

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE relname = 'pg_namespace'),
        0,
        nspacl,
        'i'
    FROM
        pg_namespace
    WHERE
        nspacl IS NOT NULL;

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class WHERE
         relname = 'pg_foreign_data_wrapper'),
        0,
        fdwacl,
        'i'
    FROM
        pg_foreign_data_wrapper
    WHERE
        fdwacl IS NOT NULL;

INSERT INTO pg_init_privs
  (objoid, classoid, objsubid, initprivs, privtype)
    SELECT
        oid,
        (SELECT oid FROM pg_class
         WHERE relname = 'pg_foreign_server'),
        0,
        srvacl,
        'i'
    FROM
        pg_foreign_server
    WHERE
        srvacl IS NOT NULL;
