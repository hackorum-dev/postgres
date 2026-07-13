--
-- Tests for PGLZ compression
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_pglz_compress(bytea)
  RETURNS bytea
  AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_pglz_decompress(bytea, int4, bool)
  RETURNS bytea
  AS :'regresslib' LANGUAGE C STRICT;
CREATE FUNCTION test_pglz_maximum_compressed_size(int4, int4)
  RETURNS int4
  AS :'regresslib' LANGUAGE C STRICT;

-- Round-trip with pglz: compress then decompress.
SELECT test_pglz_decompress(test_pglz_compress(
    decode(repeat('abcd', 100), 'escape')), 400, false) =
    decode(repeat('abcd', 100), 'escape') AS roundtrip_ok;
SELECT test_pglz_decompress(test_pglz_compress(
    decode(repeat('abcd', 100), 'escape')), 400, true) =
    decode(repeat('abcd', 100), 'escape') AS roundtrip_ok;

-- Decompression with rawsize too large, fails to fill the destination
-- buffer.
SELECT test_pglz_decompress(test_pglz_compress(
    decode(repeat('abcd', 100), 'escape')), 500, true);

-- Decompression with rawsize too small, fails with source not fully
-- consumed.
SELECT test_pglz_decompress(test_pglz_compress(
    decode(repeat('abcd', 100), 'escape')), 100, true);

-- A partial decompression only needs enough of a long literal run to fill
-- the requested output slice.  The repeated 0..255 block enters the modern
-- compressor and produces an initial 256-byte literal run.
WITH input AS
(
  SELECT decode(repeat(string_agg(lpad(to_hex(g), 2, '0'), '' ORDER BY g), 128),
                'hex') AS data
  FROM generate_series(0, 255) g
), compressed AS
(
  SELECT data, test_pglz_compress(data) AS data_compressed
  FROM input
)
SELECT get_byte(data_compressed, 6) = ascii('3') AS version_3,
       test_pglz_decompress(data_compressed, length(data), true) = data
         AS roundtrip_ok,
       test_pglz_decompress(
         substring(data_compressed FOR
           test_pglz_maximum_compressed_size(1, length(data_compressed))),
         1, false) = substring(data FOR 1) AS slice_ok
FROM compressed;

-- Corrupted version 3 data: marker without a sequence header.
SELECT test_pglz_decompress('\x01000050474c33'::bytea, 1, false);

-- Corrupted version 3 data: literal length without its extension.
SELECT test_pglz_decompress('\x01000050474c33f0'::bytea, 1, false);

-- Corrupted version 3 data: match offset exceeds the output written.
SELECT test_pglz_decompress('\x01000050474c33000000'::bytea, 6, true);

-- Corrupted compressed data.  Set control bit with read of a match tag,
-- no data follows.
SELECT length(test_pglz_decompress('\x01'::bytea, 1024, false)) AS ctrl_only_len;
SELECT test_pglz_decompress('\x01'::bytea, 1024, true);

-- Corrupted compressed data.  Set control bit with read of a match tag,
-- 1 byte follows.
SELECT test_pglz_decompress('\x01ff'::bytea, 1024, false);
SELECT test_pglz_decompress('\x01ff'::bytea, 1024, true);

-- Corrupted compressed data.  Set control bit with match tag where length
-- nibble is 3 bytes (extended length), no data follows.
SELECT test_pglz_decompress('\x010f01'::bytea, 1024, false);
SELECT test_pglz_decompress('\x010f01'::bytea, 1024, true);

-- Corrupted compressed data.  Set control bit with a valid 2-byte match
-- tag where offset exceeds output written.
SELECT test_pglz_decompress('\x011001'::bytea, 1024, false);
SELECT test_pglz_decompress('\x011001'::bytea, 1024, true);

-- Corrupted compressed data.  Set control bit with a valid 2-byte match
-- tag where offset is 0.
SELECT test_pglz_decompress('\x010300'::bytea, 1024, false);
SELECT test_pglz_decompress('\x010300'::bytea, 1024, true);

-- Clean up
DROP FUNCTION test_pglz_compress;
DROP FUNCTION test_pglz_decompress;
DROP FUNCTION test_pglz_maximum_compressed_size;
