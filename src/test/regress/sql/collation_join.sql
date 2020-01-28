CREATE TABLE raw_data (a text);
INSERT INTO raw_data (a) VALUES ('Türkiye'),
								('TÜRKIYE'),
								('bıt'),
								('BIT'),
								('äbç'),
								('ÄBÇ'),
								('aaá'),
								('coté'),
								('Götz'),
								('ὀδυσσεύς'),
								('ὈΔΥΣΣΕΎΣ'),
								('を読み取り用'),
								('にオープンできませんでした');

-- Create unpartitioned tables for test
CREATE TABLE alpha (a TEXT COLLATE "ja_JP", b TEXT COLLATE "sv_SE");
CREATE TABLE beta (a TEXT COLLATE "tr_TR", b TEXT COLLATE "en_US");

INSERT INTO alpha (SELECT a, a FROM raw_data);
INSERT INTO beta (SELECT a, a FROM raw_data);

ANALYZE alpha;
ANALYZE beta;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');

-- Try again, this time with list partitioning
DROP TABLE alpha CASCADE;
DROP TABLE beta CASCADE;

CREATE TABLE alpha (a TEXT COLLATE "ja_JP", b TEXT COLLATE "sv_SE") PARTITION BY LIST(a);
CREATE TABLE alpha_a PARTITION OF alpha FOR VALUES IN ('Türkiye', 'TÜRKIYE');
CREATE TABLE alpha_b PARTITION OF alpha FOR VALUES IN ('bıt', 'BIT');
CREATE TABLE alpha_c PARTITION OF alpha FOR VALUES IN ('äbç', 'ÄBÇ');
CREATE TABLE alpha_d PARTITION OF alpha FOR VALUES IN ('aaá', 'coté', 'Götz');
CREATE TABLE alpha_e PARTITION OF alpha FOR VALUES IN ('ὀδυσσεύς', 'ὈΔΥΣΣΕΎΣ');
CREATE TABLE alpha_f PARTITION OF alpha FOR VALUES IN ('を読み取り用', 'にオープンできませんでした', NULL);
CREATE TABLE alpha_default PARTITION OF alpha DEFAULT;

CREATE TABLE beta (a TEXT COLLATE "tr_TR", b TEXT COLLATE "en_US") PARTITION BY LIST(a);
CREATE TABLE beta_a PARTITION OF beta FOR VALUES IN ('Türkiye', 'coté', 'ὈΔΥΣΣΕΎΣ');
CREATE TABLE beta_b PARTITION OF beta FOR VALUES IN ('bıt', 'TÜRKIYE');
CREATE TABLE beta_c PARTITION OF beta FOR VALUES IN ('äbç', 'を読み取り用', 'にオープンできませんでした');
CREATE TABLE beta_d PARTITION OF beta FOR VALUES IN ('aaá', 'Götz', 'BIT', 'ὀδυσσεύς', 'ÄBÇ', NULL);
CREATE TABLE beta_default PARTITION OF beta DEFAULT;

INSERT INTO alpha (SELECT a, a FROM raw_data);
INSERT INTO beta (SELECT a, a FROM raw_data);

ANALYZE alpha;
ANALYZE beta;

EXPLAIN (COSTS OFF)
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
SELECT t1.a, t2.a FROM alpha t1 INNER JOIN beta t2 ON (t1.a = t2.a) WHERE t1.a IN ('äbç', 'ὀδυσσεύς');
