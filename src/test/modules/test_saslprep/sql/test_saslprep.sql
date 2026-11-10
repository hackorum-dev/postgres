-- Tests for SASLprep

CREATE EXTENSION test_saslprep;

-- Incomplete UTF-8 sequence.
SELECT test_saslprep('\xef');

-- Range of ASCII characters.
SELECT
    CASE
      WHEN a = 0   THEN '<NUL>'
      WHEN a < 32  THEN '<CTL_' || a::text || '>'
      WHEN a = 127 THEN '<DEL>'
      ELSE chr(a) END AS dat,
    set_byte('\x00'::bytea, 0, a) AS byt,
    test_saslprep(set_byte('\x00'::bytea, 0, a)) AS saslprep
  FROM generate_series(0,127) AS a;

-- Test SASLprep validation before and after normalization.
-- Prohibited output characters and bidi rules must be checked
-- after normalization, while code points unassigned in Unicode
-- 3.2 must be rejected before normalization.
WITH cases(name, src, expected_status, expected_res) AS (
    VALUES
    -- Characters that become valid after normalization.
    ('combining grave', 'cd80',
     'SUCCESS', 'cc80'),

    ('combining acute', 'cd81',
     'SUCCESS', 'cc81'),

    ('grave after a', '61cd80',
     'SUCCESS', 'c3a0'),

    -- Normalization can increase the number of code points.
    ('latin ligature ff', 'efac80',
     'SUCCESS', '6666'),

    -- Normalization introduces prohibited bidi sequences.
    ('hebrew presentation', 'efac9d',
     'PROHIBITED', NULL),

    ('hebrew shin', 'efacaa',
     'PROHIBITED', NULL),

    ('arabic ligature', 'efb19b',
     'PROHIBITED', NULL),

    -- Normalization changes bidi properties (L to R).
    ('alef after latin', '41e284b5',
     'PROHIBITED', NULL),

    ('alef in RTL', 'd790e284b5d791',
     'SUCCESS', 'd790d790d791'),

    -- Normalization introduces L characters into an RTL string.
    ('rupee in RTL', 'd790e282a8d791',
     'PROHIBITED', NULL),

    -- Code points unassigned in Unicode 3.2 that normalize
    -- to assigned characters using newer Unicode data.
    ('unassigned Greek', 'cfb9',
     'PROHIBITED', NULL),

    ('unassigned modifier', 'e1b4ac',
     'PROHIBITED', NULL),

    -- Control case: valid RTL string containing a digit.
    ('valid RTL digits', 'd79031d791',
     'SUCCESS', 'd79031d791')
)
SELECT name,
       actual.status,
       encode(actual.res, 'hex') AS result,
       actual.status = expected_status
         AND encode(actual.res, 'hex')
             IS NOT DISTINCT FROM expected_res AS passed
FROM cases
CROSS JOIN LATERAL test_saslprep(
    decode(cases.src, 'hex')
) AS actual
ORDER BY name;

DROP EXTENSION test_saslprep;
