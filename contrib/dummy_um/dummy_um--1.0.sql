CREATE FUNCTION dummy_user_mapping_handler(um internal)
RETURNS internal
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;