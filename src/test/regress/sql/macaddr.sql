--
-- macaddr
--

CREATE TABLE macaddr_data (a int, b macaddr);

INSERT INTO macaddr_data VALUES (1, '08:00:2b:01:02:03');
INSERT INTO macaddr_data VALUES (2, '08-00-2b-01-02-03');
INSERT INTO macaddr_data VALUES (3, '08002b:010203');
INSERT INTO macaddr_data VALUES (4, '08002b-010203');
INSERT INTO macaddr_data VALUES (5, '0800.2b01.0203');
INSERT INTO macaddr_data VALUES (6, '0800-2b01-0203');
INSERT INTO macaddr_data VALUES (7, '08002b010203');
INSERT INTO macaddr_data VALUES (8, '0800:2b01:0203'); -- invalid
INSERT INTO macaddr_data VALUES (9, 'not even close'); -- invalid

-- Overlong hex fields must be rejected, not silently wrapped (bug #19583)
SELECT '100000001:0:0:0:0:0'::macaddr;
SELECT '100000001-0-0-0-0-0'::macaddr;
SELECT '-ffffff01:0:0:0:0:0'::macaddr;
SELECT 'ffffffffffffffffff:0:0:0:0:0'::macaddr;  -- wider than any long
-- while the leniency inherited from sscanf's %x conversion is preserved
SELECT '001:00:2b:01:02:03'::macaddr;
SELECT '0x1:00:2b:01:02:03'::macaddr;
SELECT '+f:00:2b:01:02:03'::macaddr;
SELECT 'aa: bb:cc:dd:ee:ff'::macaddr;
SELECT '08:00:2b:01:02:03  '::macaddr;
-- but a bare '0x' field with no hex digit is not valid
SELECT '0x:00:2b:01:02:03'::macaddr;
-- an octet that fits the field width but is out of range is still caught
SELECT '1ff:0:0:0:0:0'::macaddr;
SELECT '-f:0:0:0:0:0'::macaddr;

INSERT INTO macaddr_data VALUES (10, '08:00:2b:01:02:04');
INSERT INTO macaddr_data VALUES (11, '08:00:2b:01:02:02');
INSERT INTO macaddr_data VALUES (12, '08:00:2a:01:02:03');
INSERT INTO macaddr_data VALUES (13, '08:00:2c:01:02:03');
INSERT INTO macaddr_data VALUES (14, '08:00:2a:01:02:04');

SELECT * FROM macaddr_data;

CREATE INDEX macaddr_data_btree ON macaddr_data USING btree (b);
CREATE INDEX macaddr_data_hash ON macaddr_data USING hash (b);

SELECT a, b, trunc(b) FROM macaddr_data ORDER BY 2, 1;

SELECT b <  '08:00:2b:01:02:04' FROM macaddr_data WHERE a = 1; -- true
SELECT b >  '08:00:2b:01:02:04' FROM macaddr_data WHERE a = 1; -- false
SELECT b >  '08:00:2b:01:02:03' FROM macaddr_data WHERE a = 1; -- false
SELECT b <= '08:00:2b:01:02:04' FROM macaddr_data WHERE a = 1; -- true
SELECT b >= '08:00:2b:01:02:04' FROM macaddr_data WHERE a = 1; -- false
SELECT b =  '08:00:2b:01:02:03' FROM macaddr_data WHERE a = 1; -- true
SELECT b <> '08:00:2b:01:02:04' FROM macaddr_data WHERE a = 1; -- true
SELECT b <> '08:00:2b:01:02:03' FROM macaddr_data WHERE a = 1; -- false

SELECT ~b                       FROM macaddr_data;
SELECT  b & '00:00:00:ff:ff:ff' FROM macaddr_data;
SELECT  b | '01:02:03:04:05:06' FROM macaddr_data;

DROP TABLE macaddr_data;

-- test non-error-throwing API for some core types
SELECT pg_input_is_valid('08:00:2b:01:02:ZZ', 'macaddr');
SELECT * FROM pg_input_error_info('08:00:2b:01:02:ZZ', 'macaddr');
SELECT pg_input_is_valid('08:00:2b:01:02:', 'macaddr');
SELECT * FROM pg_input_error_info('08:00:2b:01:02:', 'macaddr');
-- overlong hex field (bug #19583)
SELECT pg_input_is_valid('100000001:0:0:0:0:0', 'macaddr');
SELECT * FROM pg_input_error_info('100000001:0:0:0:0:0', 'macaddr');
