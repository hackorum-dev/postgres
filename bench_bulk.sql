\set id_start random(100000000, 2000000000)
BEGIN;
SELECT lo_bulk_put(
    (SELECT array_agg(id::oid) FROM generate_series(:id_start, :id_start + 999) id),
    (SELECT array_agg(decode(repeat('01', 10240), 'hex')) FROM generate_series(1, 1000))
);
ROLLBACK;