# Read-write-unique test.
# From bug report:
# https://postgr.es/m/CAGPCyEZG76zjv7S31v_xPeLNRuzj-m%3DY2GOY7PEzu7vhB%3DyQog%40mail.gmail.com

setup
{
CREATE TABLE t (
    item_id INT NOT NULL,
    version INT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL,
    UNIQUE (item_id, version),
    UNIQUE (item_id, created_at)
);
INSERT INTO t (item_id, version, created_at) VALUES
    (10, 1, now() - INTERVAL '2 SECOND'),
    (10, 2, now() - INTERVAL '1 SECOND');
}

teardown
{
  DROP TABLE t;
}

session "s1"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "r1" { 
  SELECT version FROM t
   WHERE NOT EXISTS (SELECT 1 FROM t t2
                      WHERE t.item_id = t2.item_id
                        AND t.created_at < t2.created_at)
     AND item_id = 10;
}
step "w1" {
  INSERT INTO t (item_id, version, created_at)
  VALUES (10, 3, now());
}
step "c1" { COMMIT; }

session "s2"
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step "r2" { 
  SELECT version FROM t
   WHERE NOT EXISTS (SELECT 1 FROM t t2
                      WHERE t.item_id = t2.item_id
                        AND t.created_at < t2.created_at)
     AND item_id = 10;
}
step "w2" {
  INSERT INTO t (item_id, version, created_at)
  VALUES (10, 3, now());
}
step "c2" { COMMIT; }

# XXX This should ideally detect serialization failure before UCV
permutation "r1" "r2" "w1" "w2" "c1" "c2"
