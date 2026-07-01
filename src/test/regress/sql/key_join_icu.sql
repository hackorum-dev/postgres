--
-- key_join_icu
--
-- FOR KEY join cases that depend on an ICU nondeterministic collation.  Such a
-- collation can only be created when the database encoding is one that ICU
-- supports (e.g. UTF8), so these cases used to live inline in key_join.sql and
-- key_join_mcdc.sql and broke the regression suite on SQL_ASCII databases and
-- on builds without ICU.  Keep them here and skip the whole file unless an ICU
-- collation can actually be built; the proof behavior they exercise is
-- collation-mechanism specific and not otherwise reachable without ICU.
--

/* skip test if not UTF8 server encoding or no ICU collations installed */
SELECT getdatabaseencoding() <> 'UTF8' OR
       (SELECT count(*) FROM pg_collation WHERE collprovider = 'i' AND collname <> 'unicode') = 0
       AS skip_test \gset
\if :skip_test
\quit
\endif

CREATE SCHEMA key_join_icu;
SET search_path = key_join_icu, public;

-- Exact-matching nondeterministic collations are a usable equality identity
-- for catalog and query-shape proof facts.  The byte-distinct spellings below
-- compare equal at ICU primary strength.
CREATE COLLATION key_nondet (provider = icu, locale = 'und-u-ks-level1',
    deterministic = false);

SELECT 'Alpha'::text COLLATE key_nondet =
       'alpha'::text COLLATE key_nondet AS collates_equal,
       convert_to('Alpha', 'UTF8') <>
       convert_to('alpha', 'UTF8') AS bytes_differ;

CREATE TABLE key_nondet_parent
(
    code text COLLATE key_nondet PRIMARY KEY
);
CREATE TABLE key_nondet_child
(
    id          int PRIMARY KEY,
    code        text COLLATE key_nondet UNIQUE NOT NULL,
    parent_code text COLLATE key_nondet NOT NULL
        REFERENCES key_nondet_parent (code)
);
CREATE TABLE key_nondet_grandchild
(
    id         int PRIMARY KEY,
    child_code text COLLATE key_nondet NOT NULL
        REFERENCES key_nondet_child (code)
);

INSERT INTO key_nondet_parent VALUES ('Alpha'), ('BETA');
INSERT INTO key_nondet_child VALUES
    (10, 'Child-One', 'alpha'),
    (20, 'CHILD-TWO', 'beta'),
    (30, 'Child-Three', 'ALPHA');
INSERT INTO key_nondet_grandchild VALUES
    (100, 'child-one'),
    (200, 'Child-Two');

-- accepted: the unique and FK constraints use the same nondeterministic
-- equality, and the displayed values prove that matching is not bytewise.
SELECT c.id, p.code AS parent_code, c.parent_code AS referenced_as
FROM key_nondet_parent p
JOIN key_nondet_child c FOR KEY p (code) <- c (parent_code)
ORDER BY c.id;

-- accepted: both FOR KEY joins propagate exact nondeterministic equality
-- identities through the intermediate join surface.
SELECT g.id, c.code AS child_code, p.code AS parent_code
FROM key_nondet_parent p
JOIN key_nondet_child c FOR KEY p (code) <- c (parent_code)
JOIN key_nondet_grandchild g FOR KEY c (code) <- g (child_code)
ORDER BY g.id;

-- accepted: the inner key join preserves the child's nondeterministic unique
-- fact and row coverage through a derived table.
SELECT g.id, d.child_code, d.parent_code
FROM (
    SELECT c.code AS child_code, p.code AS parent_code
    FROM key_nondet_parent p
    JOIN key_nondet_child c FOR KEY p (code) <- c (parent_code)
) d
JOIN key_nondet_grandchild g FOR KEY d (child_code) <- g (child_code)
ORDER BY g.id;

-- The LEFT JOIN preserves all parent keys but may duplicate them.  DISTINCT
-- under the same nondeterministic equality must therefore supply the unique
-- proof fact needed by the outer key join; the base unique fact cannot do so.
SELECT c.id, q.code
FROM (
    SELECT DISTINCT p.code
    FROM key_nondet_parent p
    LEFT JOIN key_nondet_child d FOR KEY p (code) <- d (parent_code)
) q
JOIN key_nondet_child c FOR KEY q (code) <- c (parent_code)
ORDER BY c.id;

-- GROUP BY exercises the corresponding query-level unique proof path.
SELECT c.id, q.code
FROM (
    SELECT p.code
    FROM key_nondet_parent p
    LEFT JOIN key_nondet_child d FOR KEY p (code) <- d (parent_code)
    GROUP BY p.code
) q
JOIN key_nondet_child c FOR KEY q (code) <- c (parent_code)
ORDER BY c.id;

-- Matching filters using the key's exact nondeterministic equality preserve
-- row coverage.
CREATE VIEW key_nondet_parent_filtered AS
SELECT code FROM key_nondet_parent
WHERE code = 'alpha'::text COLLATE key_nondet;

CREATE VIEW key_nondet_child_filtered AS
SELECT id, parent_code FROM key_nondet_child
WHERE parent_code = 'alpha'::text COLLATE key_nondet;

SELECT c.id, p.code, c.parent_code
FROM key_nondet_parent_filtered p
JOIN key_nondet_child_filtered c FOR KEY p (code) <- c (parent_code)
ORDER BY c.id;

DROP VIEW key_nondet_child_filtered, key_nondet_parent_filtered;

-- A distinct nondeterministic collation OID is not interchangeable, even when
-- its provider, locale, and options are identical.
CREATE COLLATION key_nondet_copy (provider = icu,
    locale = 'und-u-ks-level1', deterministic = false);

CREATE VIEW key_nondet_parent_copy_filtered AS
SELECT code FROM key_nondet_parent
WHERE code = 'alpha'::text COLLATE key_nondet_copy;

CREATE VIEW key_nondet_child_copy_filtered AS
SELECT id, parent_code FROM key_nondet_child
WHERE parent_code = 'alpha'::text COLLATE key_nondet_copy;

-- rejected, reason: nondeterministic proof identities require the same OID
SELECT c.id, p.code, c.parent_code
FROM key_nondet_parent_copy_filtered p
JOIN key_nondet_child_copy_filtered c FOR KEY p (code) <- c (parent_code);

DROP VIEW key_nondet_child_copy_filtered, key_nondet_parent_copy_filtered;
DROP COLLATION key_nondet_copy;

-- The mirror image of the filter_collation case below: a deterministic filter
-- collation on a nondeterministic key.  Bytewise equality keeps only the
-- spelling 'alpha', so the filtered parent loses the row 'Alpha' that the
-- key's equality still matches for the filtered child.
CREATE VIEW key_nondet_parent_c_filtered AS
SELECT code FROM key_nondet_parent
WHERE code = 'alpha'::text COLLATE "C";

CREATE VIEW key_nondet_child_c_filtered AS
SELECT id, parent_code FROM key_nondet_child
WHERE parent_code = 'alpha'::text COLLATE "C";

SELECT (SELECT count(*) FROM key_nondet_parent_c_filtered) AS parent_rows,
       (SELECT count(*) FROM key_nondet_child_c_filtered) AS child_rows;

-- rejected, reason: deterministic filter collation disagrees with the key's equality
SELECT c.id, p.code, c.parent_code
FROM key_nondet_parent_c_filtered p
JOIN key_nondet_child_c_filtered c FOR KEY p (code) <- c (parent_code);

DROP VIEW key_nondet_child_c_filtered, key_nondet_parent_c_filtered;
DROP TABLE key_nondet_grandchild, key_nondet_child, key_nondet_parent;
DROP COLLATION key_nondet;

-- A nondeterministic filter collation may disagree on equality, so a filtered
-- referenced relation cannot be proven covered (originally in key_join.sql).
CREATE TABLE filter_collation_parent
(
    code text COLLATE "C" PRIMARY KEY
);
CREATE TABLE filter_collation_child
(
    code text COLLATE "C" NOT NULL
        REFERENCES filter_collation_parent (code)
);

INSERT INTO filter_collation_parent VALUES ('a'), ('b');
INSERT INTO filter_collation_child VALUES ('a'), ('b');

CREATE COLLATION filter_collation_nondet (provider = icu, locale = 'und',
    deterministic = false);

CREATE VIEW filter_collation_parent_nondet AS
SELECT code FROM filter_collation_parent
WHERE code = 'a'::text COLLATE filter_collation_nondet;

CREATE VIEW filter_collation_child_nondet AS
SELECT code FROM filter_collation_child
WHERE code = 'a'::text COLLATE filter_collation_nondet;

-- rejected, reason: nondeterministic filter collation may disagree on equality
SELECT *
FROM filter_collation_parent_nondet p
JOIN filter_collation_child_nondet c FOR KEY p (code) <- c (code);

DROP VIEW filter_collation_child_nondet, filter_collation_parent_nondet;
DROP COLLATION filter_collation_nondet;
DROP TABLE filter_collation_child, filter_collation_parent;

DROP SCHEMA key_join_icu;
