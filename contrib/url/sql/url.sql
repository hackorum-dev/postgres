--
-- Basic URL tests for the behavior of functions.
-- The tests for compliance with the specification are located separately.
--

-- The tests are designed for a UTF-8 database.  Skip otherwise.
SELECT getdatabaseencoding() NOT IN ('UTF8')
  AS skip_test \gset
\if :skip_test
  \quit
\endif

SELECT getdatabaseencoding(); -- label the results files

CREATE EXTENSION url;

-- Getters
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).scheme; -- OK, https
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).username; -- OK, root
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).password; -- OK, qwerty
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).host; -- OK, example.com
select ('https://root:qwerty@εxαmπle.cθm:8080/path/to/home?abc=xyz#anchor'::url).host; -- OK, xn--xmle-0ldw4f.xn--cm-x9b
select ('https://root:qwerty@εxαmπle.cθm:8080/path/to/home?abc=xyz#anchor'::url).host_unicode; -- OK, εxαmπle.cθm
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).port; -- OK, 8080
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).path; -- OK, /path/to/home
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).query; -- OK, abc=xyz
select ('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url).fragment; -- OK, anchor

-- Setters

select url_scheme_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, 'wss'); -- OK
select url_username_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, 'guest'); -- OK
select url_password_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, '12345'); -- OK
select url_host_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, 'postgresql.org'); -- OK
select url_port_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, '80'); -- OK
select url_path_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, '/docs/books/'); -- OK
select url_query_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, 'xyz=abc'); -- OK
select url_fragment_set('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, 'general_questions'); -- OK

-- Base
select url_base(NULL::url, NULL); -- OK, NULL
select url_base('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, NULL); -- OK
select url_base('https://root:qwerty@example.com:8080/path/to/home?abc=xyz#anchor'::url, '/change/path'); -- OK

-- Unicode
select ('https://εxαmπle.cθm/'::url).host; -- OK, xn--xmle-0ldw4f.xn--cm-x9b
select ('https://εxαmπle.cθm/'::url).host_unicode; -- OK, εxαmπle.cθm

-- Percent Encode
select ('https://βεst@example.com'::url).username; -- OK
select ('https://:παssφord@example.com'::url).password; -- OK
select ('https://example.com/pαth/to/hθmε'::url).path; -- OK
select ('https://xample.com/?αβγ=χψω'::url).query; -- OK
select ('https://xample.com/#αnchθrΩ'::url).fragment; -- OK

-- Getters Ok, Error
select ''::url;  -- ERROR
select NULL::url; -- OK

select (NULL::url).scheme; -- OK
select ('file://path/to'::url).scheme; -- OK
select ('/bad/url'::url).scheme; -- ERROR

select (NULL::url).username; -- OK
select ('https://example.com'::url).username; -- OK

select (NULL::url).password; -- OK
select ('https://example.com'::url).password; -- OK

select (NULL::url).host; -- OK
select ('file://host/to'::url).host; -- OK
select ('file:/path/to'::url).host; -- OK
select (NULL::url).host_unicode; -- OK
select ('file://host/to'::url).host_unicode; -- OK
select ('file:/path/to'::url).host_unicode; -- OK

select (NULL::url).port; -- OK
select ('https://example.com'::url).port; -- OK

select (NULL::url).path; -- OK
select ('https://example.com'::url).path; -- OK
select ('file:/path/to'::url).path; -- OK

select (NULL::url).query; -- OK
select ('https://example.com'::url).query; -- OK

select (NULL::url).fragment; -- OK
select ('https://example.com'::url).fragment; -- OK

-- Setters Ok, Error
select url_scheme_set('https://example.com'::url, NULL); -- ERROR
select url_scheme_set('https://example.com'::url, ''); -- ERROR
select url_scheme_set('https://example.com'::url, '---+'); -- ERROR

select url_username_set('https://root:qwerty@example.com'::url, NULL); -- OK
select url_username_set('https://root:qwerty@example.com'::url, ''); -- OK
select url_username_set('https://root:qwerty@example.com'::url, 'αβγ'); -- OK

select url_password_set('https://root:qwerty@example.com'::url, NULL); -- OK
select url_password_set('https://root:qwerty@example.com'::url, ''); -- OK
select url_password_set('https://root:qwerty@example.com'::url, 'αβγ'); -- OK

select url_port_set('https://example.com:8080'::url, NULL); -- OK
select url_port_set('https://example.com:8080'::url, ''); -- OK
select url_port_set('https://example.com:8080'::url, '80'); -- OK
select url_port_set('https://example.com:8080'::url, '123456'); -- ERROR
select url_port_set('https://example.com:8080'::url, 80); -- OK
select url_port_set('https://example.com:8080'::url, 123456); -- ERROR

select url_host_set('https://example.com'::url, NULL); -- ERROR
select url_host_set('https://example.com'::url, ''); -- ERROR
select url_host_set('https://example.com'::url, '123'); -- OK
select url_host_set('https://example.com'::url, 'αβγ'); -- OK

select url_path_set('https://example.com/path/to/home'::url, NULL); -- OK
select url_path_set('https://example.com/path/to/home'::url, ''); -- OK
select url_path_set('https://example.com/path/to/home'::url, '/'); -- OK
select url_path_set('https://example.com/path/to/home'::url, 'αβγ'); -- OK

select url_query_set('https://example.com?abc=xyz'::url, NULL); -- OK
select url_query_set('https://example.com?abc=xyz'::url, ''); -- OK
select url_query_set('https://example.com?abc=xyz'::url, 'αβγ'); -- OK

select url_fragment_set('https://example.com#anchor'::url, NULL); -- OK
select url_fragment_set('https://example.com#anchor'::url, ''); -- OK
select url_fragment_set('https://example.com#anchor'::url, 'αβγ'); -- OK
