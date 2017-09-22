SET client_encoding TO UTF8;

CREATE TABLE names (name text);

INSERT INTO names VALUES ('Göbel'), ('Goethe'), ('Goldmann'), ('Göthe'), ('Götz');

CREATE COLLATION "de-u-co-phonebk-x-icu/1" (provider = icu, locale = 'de-u-co-phonebk');
CREATE COLLATION "de-u-co-phonebk-x-icu/2" (provider = icu, locale = 'de@collation=phonebook');

SELECT name FROM names ORDER BY name COLLATE "de-x-icu";
SELECT name FROM names ORDER BY name COLLATE "de-u-co-phonebk-x-icu/1";
SELECT name FROM names ORDER BY name COLLATE "de-u-co-phonebk-x-icu/2";


CREATE COLLATION "und-u-co-emoji-x-icu/1" (provider = icu, locale = 'und-u-co-emoji');
CREATE COLLATION "und-u-co-emoji-x-icu/2" (provider = icu, locale = '@collation=emoji');

SELECT chr(x) FROM generate_series(x'1F634'::int, x'1F644'::int) AS _(x) ORDER BY chr(x) COLLATE "und-x-icu";
SELECT chr(x) FROM generate_series(x'1F634'::int, x'1F644'::int) AS _(x) ORDER BY chr(x) COLLATE "und-u-co-emoji-x-icu/1";
SELECT chr(x) FROM generate_series(x'1F634'::int, x'1F644'::int) AS _(x) ORDER BY chr(x) COLLATE "und-u-co-emoji-x-icu/2";


CREATE TABLE test1 (x text);
INSERT INTO test1 VALUES ('1'), ('12'), ('123'), ('2'), ('21'), ('a'), ('b'), ('c'), ('A'), ('B'), ('C');

CREATE COLLATION digitslast1 (provider = icu, locale = 'en-u-kr-latn-digit');
CREATE COLLATION digitslast2 (provider = icu, locale = 'en@colReorder=latn-digit');

SELECT * FROM test1 ORDER BY x COLLATE "und-x-icu";
SELECT * FROM test1 ORDER BY x COLLATE digitslast1;
SELECT * FROM test1 ORDER BY x COLLATE digitslast2;

CREATE COLLATION upperfirst1 (provider = icu, locale = 'en-u-kf-upper');
CREATE COLLATION upperfirst2 (provider = icu, locale = 'en@colCaseFirst=upper');

SELECT * FROM test1 ORDER BY x COLLATE upperfirst1;
SELECT * FROM test1 ORDER BY x COLLATE upperfirst2;

CREATE COLLATION special1 (provider = icu, locale = 'en-u-kf-upper-kr-latn-digit');
CREATE COLLATION special2 (provider = icu, locale = 'en@colCaseFirst=upper;colReorder=latn-digit');

SELECT * FROM test1 ORDER BY x COLLATE special1;
SELECT * FROM test1 ORDER BY x COLLATE special2;

CREATE COLLATION numeric1 (provider = icu, locale = 'en-u-kn-true');
CREATE COLLATION numeric2 (provider = icu, locale = 'en@colNumeric=yes');

SELECT * FROM test1 ORDER BY x COLLATE numeric1;
SELECT * FROM test1 ORDER BY x COLLATE numeric2;
