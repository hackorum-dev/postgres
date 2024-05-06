/* 
 * The file is used to test cast_jsonb_to_hstore.sql
*/
CREATE EXTENSION hstore;
CREATE EXTENSION cast_jsonb_to_hstore;

SELECT '{"aaa":"absr"}'::jsonb::hstore;

SELECT '{"aaa":"absr"}'::jsonb::hstore->'aaa';

SELECT '{"aaa":1234}'::jsonb::hstore;

SELECT '{"aaa":1234}'::jsonb::hstore->'aaa';

SELECT '{"aaa":true}'::jsonb::hstore;

SELECT '{"aaa":true}'::jsonb::hstore->'aaa';

SELECT '{"aaa":null}'::jsonb::hstore;

SELECT '{"aaa":null}'::jsonb::hstore->'aaa';

SELECT '{"1234":"absr"}'::jsonb::hstore;

SELECT E'{"a": "\'ght"}'::jsonb::hstore;

SELECT E'{"a": "\'ght"}'::jsonb::hstore->'a';