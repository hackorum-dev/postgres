CREATE EXTENSION test_utf8_validate;

SELECT drive_utf8_validate(-1);

\timing on

SELECT drive_utf8_validate(1);
SELECT drive_utf8_validate(2);
SELECT drive_utf8_validate(4);
SELECT drive_utf8_validate(8);
SELECT drive_utf8_validate(16);
SELECT drive_utf8_validate(32);
SELECT drive_utf8_validate(64);
SELECT drive_utf8_validate(128);
SELECT drive_utf8_validate(256);
SELECT drive_utf8_validate(512);
SELECT drive_utf8_validate(1024);