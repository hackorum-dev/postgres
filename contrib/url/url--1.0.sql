CREATE TYPE url;


CREATE FUNCTION url_in(cstring) RETURNS url
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS '$libdir/url';

CREATE FUNCTION url_out(url) RETURNS cstring
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS '$libdir/url';

CREATE TYPE url (
    INTERNALLENGTH = -1,
    INPUT = url_in,
    OUTPUT = url_out
);


CREATE CAST (url AS text) WITH INOUT AS ASSIGNMENT;
CREATE CAST (text AS url) WITH INOUT AS ASSIGNMENT;


CREATE FUNCTION scheme(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_scheme';

CREATE FUNCTION username(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_username';

CREATE FUNCTION password(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_password';

CREATE FUNCTION port(url) RETURNS integer
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_port';

CREATE FUNCTION host(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_host';

CREATE FUNCTION host_unicode(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_host_unicode';

CREATE FUNCTION path(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_path';

CREATE FUNCTION query(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_query';

CREATE FUNCTION fragment(url) RETURNS text
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_fragment';

CREATE FUNCTION to_url(text) RETURNS url
    IMMUTABLE
    STRICT
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_create';

CREATE FUNCTION url_base(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_base';


CREATE FUNCTION url_scheme_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_scheme_set';

CREATE FUNCTION url_username_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_username_set';

CREATE FUNCTION url_password_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_password_set';

CREATE FUNCTION url_host_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_host_set';

CREATE FUNCTION url_hostname_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_hostname_set';

CREATE FUNCTION url_port_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_port_set';

CREATE FUNCTION url_port_set(url, integer) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_port_num_set';

CREATE FUNCTION url_path_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_path_set';

CREATE FUNCTION url_query_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_query_set';

CREATE FUNCTION url_fragment_set(url, text) RETURNS url
    IMMUTABLE
    LANGUAGE C
    AS 'MODULE_PATHNAME','url_fragment_set';
