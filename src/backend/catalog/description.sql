/* Create default descriptions for operator implementation functions */
WITH funcdescs AS (
    SELECT p.oid as p_oid, o.oid as o_oid, oprname
    FROM pg_proc p  JOIN pg_operator o ON oprcode = p.oid
)
INSERT INTO pg_description
    SELECT p_oid, 'pg_proc'::regclass, 0,
        'implementation of ' || oprname || ' operator'
    FROM funcdescs
    WHERE NOT EXISTS (
           SELECT 1 FROM pg_description
           WHERE objoid = p_oid AND classoid = 'pg_proc'::regclass)
        AND NOT EXISTS (
	    SELECT 1 FROM pg_description
            WHERE objoid = o_oid AND classoid = 'pg_operator'::regclass
                AND description LIKE 'deprecated%');
