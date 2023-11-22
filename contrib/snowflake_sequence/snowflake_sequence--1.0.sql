/* contrib/snowflake_sequence/snowflake_sequence--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION snowflake_sequence" to load this file. \quit

-- Create a small table to record a Machine ID.
CREATE TABLE snowflake_sequence.machine_id (
    identifier smallint CHECK (identifier > 0),
    CHECK (identifier < 512)
) WITH (user_catalog_table=true);

-- Set a default value of the ID. Here, a random value is used.
INSERT INTO snowflake_sequence.machine_id
    SELECT round((random() * (0 - 511))::numeric, 0) + 511;

-- Create a snowflake sequence. Actually, this function just creates a normal
-- sequence in the snowflake_sequence schema. Other parts in snowflake id is
-- dynamically calculated so that we do not have to do anything.
CREATE FUNCTION snowflake_sequence.create_sequence(sequence_name name)
RETURNS void AS $$
BEGIN
    EXECUTE 'CREATE SEQUENCE snowflake_sequence.' || sequence_name || ' AS int MINVALUE 1 MAXVALUE 4095 CYCLE';
END;
$$ LANGUAGE plpgsql;


-- Returns a nextval counted by a snowflake sequence. 
CREATE FUNCTION snowflake_sequence.nextval(sequence_name name)
RETURNS bigint AS $$
DECLARE
    machine_id smallint;
    ret bigint;
BEGIN
    SELECT identifier FROM snowflake_sequence.machine_id INTO machine_id;
    SELECT snowflake_sequence.snowflake_nextval_internal(sequence_name::text, machine_id) INTO ret;

    return ret;
END;
$$ LANGUAGE plpgsql;

-- Internal function for nextval. Should not be called from users.
CREATE FUNCTION snowflake_sequence.snowflake_nextval_internal(sequence_name text, machine_id int)
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C;
REVOKE EXECUTE ON FUNCTION snowflake_sequence.snowflake_nextval_internal(text, int) FROM PUBLIC;
