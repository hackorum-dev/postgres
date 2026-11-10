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

-- Test prohibited code points and bidi rules after normalization
WITH cases(name, src, expected_status, expected_res) AS (
    VALUES
    ('step3', decode('61cd80', 'hex'),
     'SUCCESS', decode('c3a0', 'hex')),
    ('step4', decode('efac9d', 'hex'),
     'PROHIBITED', NULL::bytea)
)
SELECT name,
       status,
       encode(res, 'hex') AS result,
       status = expected_status
           AND res IS NOT DISTINCT FROM expected_res AS passed
FROM cases
CROSS JOIN LATERAL test_saslprep(src);

DROP EXTENSION test_saslprep;
