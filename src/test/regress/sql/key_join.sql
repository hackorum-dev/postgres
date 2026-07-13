--
-- FOR KEY joins
--
-- Every case uses this one model.  A case that needs a different catalog
-- state makes the change in a transaction and rolls it back.
--
-- customers and orders each lose a column ahead of their keys, so that
-- throughout the file their column numbers differ from the positions that
-- SELECT * yields.
--

CREATE TABLE customers (old_code int, id int PRIMARY KEY, name text NOT NULL);
CREATE TABLE orders (
    old_ref int,
    id int PRIMARY KEY,
    customer_id int NOT NULL REFERENCES customers,
    referrer_id int REFERENCES customers
);
CREATE TABLE order_items (
    order_id int NOT NULL REFERENCES orders,
    item_no int NOT NULL,
    PRIMARY KEY (order_id, item_no)
);
CREATE TABLE shipments (
    id int PRIMARY KEY,
    order_id int NOT NULL,
    item_no int NOT NULL,
    FOREIGN KEY (order_id, item_no) REFERENCES order_items
);
ALTER TABLE customers DROP old_code;
ALTER TABLE orders DROP old_ref;

INSERT INTO customers VALUES (1, 'A Ltd'), (2, 'B Ltd'), (3, 'C Ltd');
INSERT INTO orders VALUES (10, 1, NULL), (11, 2, 1);
INSERT INTO order_items VALUES (10, 1), (10, 2), (11, 1);
INSERT INTO shipments VALUES (100, 10, 2);

-- The arrow, not the operand order, says which side references.
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
SELECT FROM customers c JOIN orders o FOR KEY c (id) <- o (customer_id);

-- Composite keys match by column pairs, so the pairs may come in any order.
SELECT s.id, i.order_id, i.item_no
FROM shipments s JOIN order_items i FOR KEY s (item_no, order_id) -> i (item_no, order_id);

-- A nullable foreign key is proven only if the referencing side is preserved.
SELECT FROM orders o LEFT JOIN customers c
  FOR KEY o (referrer_id) -> c (id);
SELECT FROM customers c RIGHT JOIN orders o
  FOR KEY c (id) <- o (referrer_id);
SELECT FROM orders o FULL JOIN customers c
  FOR KEY o (referrer_id) -> c (id);
-- rejected, reason: the inner join would drop rows with a null referrer
SELECT FROM orders o JOIN customers c FOR KEY o (referrer_id) -> c (id);

-- rejected, reason: the arrow points at the referencing side
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) <- c (id);
-- rejected, reason: the columns are not the foreign key's referenced columns
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (name);
-- rejected, reason: no foreign key over these referencing columns
SELECT FROM orders o JOIN customers c FOR KEY o (id) -> c (id);
-- rejected, reason: only part of a composite key
SELECT FROM shipments s JOIN order_items i FOR KEY s (order_id) -> i (order_id);
-- rejected, reason: the column pairs do not follow the foreign key
SELECT FROM shipments s JOIN order_items i
  FOR KEY s (order_id, item_no) -> i (item_no, order_id);

-- rejected, reason: column list lengths differ
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id, id) -> c (id);
-- rejected, reason: an endpoint must name a table of its own operand, so the
-- endpoints follow the operand order
SELECT FROM orders o JOIN customers c FOR KEY c (id) <- o (customer_id);
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> o (id);
-- rejected, reason: unknown column, on either side
SELECT FROM orders o JOIN customers c FOR KEY o (x) -> c (id);
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (x);
-- rejected, reason: USING merged the endpoint's column away
SELECT FROM orders a JOIN orders b USING (customer_id)
JOIN customers c FOR KEY b (customer_id) -> c (id);
-- rejected, reason: the operand exposes two columns with this name
SELECT FROM orders o JOIN (SELECT id, name AS id FROM customers) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: the arrow cannot be split
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) < - c (id);

-- rejected, reason: two visible relations are named customers
BEGIN;
CREATE TEMP TABLE customers (id int);
SELECT FROM orders o JOIN (customers CROSS JOIN public.customers)
  FOR KEY o (customer_id) -> customers (id);
ROLLBACK;

-- rejected, reason: a foreign key that is not validated
BEGIN;
ALTER TABLE orders DROP CONSTRAINT orders_customer_id_fkey;
ALTER TABLE orders ADD FOREIGN KEY (customer_id) REFERENCES customers NOT VALID;
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- rejected, reason: a foreign key that is not enforced
BEGIN;
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey NOT ENFORCED;
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- rejected, reason: a deferrable foreign key
BEGIN;
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey DEFERRABLE;
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- rejected, reason: row-level security can hide referenced rows
BEGIN;
ALTER TABLE customers ENABLE ROW LEVEL SECURITY;
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- rejected, reason: sampling can skip referenced rows
SELECT FROM orders o JOIN customers c TABLESAMPLE SYSTEM (100)
  FOR KEY o (customer_id) -> c (id);

-- An inheritance child is not covered by the parent's key, but ONLY avoids it.
BEGIN;
CREATE TABLE vip_customers () INHERITS (customers);
SELECT FROM orders o JOIN ONLY customers c FOR KEY o (customer_id) -> c (id);
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- A cached plan is not reused once its proof no longer holds, and works
-- again when the proof is restored.
PREPARE order_names AS
SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
EXECUTE order_names;
BEGIN;
CREATE TABLE vip_customers () INHERITS (customers);
EXECUTE order_names;
ROLLBACK;
EXECUTE order_names;
DEALLOCATE order_names;

-- A partition inherits a usable foreign key from its parent.
BEGIN;
CREATE TABLE invoices (customer_id int NOT NULL REFERENCES customers)
  PARTITION BY RANGE (customer_id);
CREATE TABLE invoices_low PARTITION OF invoices FOR VALUES FROM (0) TO (10);
SELECT FROM invoices v JOIN customers c FOR KEY v (customer_id) -> c (id);
SELECT FROM invoices_low v JOIN customers c FOR KEY v (customer_id) -> c (id);
ROLLBACK;

-- Endpoints may be schema-qualified, and the qualification selects between
-- tables of the same name.
BEGIN;
CREATE SCHEMA archive;
CREATE TABLE archive.orders (id int PRIMARY KEY REFERENCES public.orders);
SELECT FROM public.orders JOIN archive.orders
  FOR KEY public.orders (id) <- archive.orders (id);
-- rejected, reason: archive.orders is not in the left operand
SELECT FROM public.orders JOIN archive.orders
  FOR KEY archive.orders (id) -> public.orders (id);
ROLLBACK;

-- An endpoint is not a table reference for WITH RECURSIVE dependency analysis.
WITH RECURSIVE orders AS (
  SELECT orders.id
  FROM public.orders AS orders JOIN customers
    FOR KEY orders (customer_id) -> customers (id)
)
SELECT FROM orders;

-- rejected, reason: an alias hides the qualified name, even an identical alias
SELECT FROM public.orders AS orders JOIN customers
  FOR KEY public.orders (customer_id) -> customers (id);

-- rejected, reason: no such schema, reported as a quoted qualified name
SELECT FROM orders JOIN customers
  FOR KEY "No.Such".orders (customer_id) -> customers (id);

-- rejected, reason: another database
SELECT FROM orders JOIN customers
  FOR KEY other_db.public.orders (customer_id) -> customers (id);

-- rejected, reason: function bodies and policies are not rechecked yet
BEGIN;
CREATE FUNCTION stored_function() RETURNS bigint
BEGIN ATOMIC
  SELECT count(*) FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
END;
ROLLBACK;
BEGIN;
CREATE POLICY stored_policy ON shipments USING (EXISTS (SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id)));
ROLLBACK;

-- An endpoint can be reached through any operand that passes its rows and
-- key columns through unchanged: a join, a subquery with renamed columns, a
-- CTE and a view.
SELECT FROM order_items i
JOIN orders o FOR KEY i (order_id) -> o (id)
JOIN customers c FOR KEY o (customer_id) -> c (id);

SELECT FROM orders o
JOIN (SELECT id AS customer_key FROM customers) c
  FOR KEY o (customer_id) -> c (customer_key);

WITH c AS (SELECT * FROM customers)
SELECT FROM orders o JOIN c FOR KEY o (customer_id) -> c (id);

BEGIN;
CREATE VIEW customer_names AS SELECT id, name FROM customers;
SELECT FROM orders o JOIN customer_names c
  FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- Only the referenced side must be complete; the referencing side may be
-- filtered.
SELECT FROM (SELECT * FROM orders WHERE id = 11) o
JOIN customers c FOR KEY o (customer_id) -> c (id);
-- Grouping the referencing side keeps its values, so it still references.
SELECT FROM (SELECT customer_id FROM orders GROUP BY customer_id) g
JOIN customers c FOR KEY g (customer_id) -> c (id);

-- rejected, reason: the referenced input is filtered
SELECT FROM orders o JOIN (SELECT id FROM customers WHERE id > 1) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: the referenced input is sampled
SELECT FROM orders o
JOIN (SELECT id FROM customers TABLESAMPLE SYSTEM (100)) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: a key expression is not the key column
SELECT FROM orders o JOIN (SELECT id + 0 AS id FROM customers) c
  FOR KEY o (customer_id) -> c (id);
-- Every order keeps exactly one row through its own key join to customers,
-- so that join can still be the referenced side.
SELECT FROM order_items i
JOIN (customers c JOIN orders o FOR KEY c (id) <- o (customer_id))
  FOR KEY i (order_id) -> o (id);
-- An endpoint can be a join alias rather than a table.
SELECT FROM (orders o JOIN customers c FOR KEY o (customer_id) -> c (id)) AS j(order_id)
LEFT JOIN customers r FOR KEY j (referrer_id) -> r (id);
-- rejected, reason: an inner join to a child can drop referenced rows
BEGIN;
CREATE TABLE loyalty_cards (customer_id int PRIMARY KEY REFERENCES customers);
SELECT FROM orders o
JOIN (SELECT c.id FROM customers c
      JOIN loyalty_cards l FOR KEY c (id) <- l (customer_id)) c
  FOR KEY o (customer_id) -> c (id);
ROLLBACK;
-- rejected, reason: the row loss happens inside a view, and the detail says so
BEGIN;
CREATE VIEW top_customers AS SELECT id FROM customers LIMIT 1;
SELECT FROM orders o JOIN top_customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;
-- rejected, reason: an earlier join repeats referenced rows
SELECT FROM orders o
JOIN (customers c JOIN orders o2 FOR KEY c (id) <- o2 (customer_id))
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: an outer join can null-extend the referencing column
SELECT FROM (customers c LEFT JOIN orders o FOR KEY c (id) <- o (customer_id))
JOIN customers c2 FOR KEY o (customer_id) -> c2 (id);
-- rejected, reason: a set operation's output has no proven key
SELECT FROM orders o
JOIN (SELECT id FROM customers UNION ALL SELECT id FROM customers) c
  FOR KEY o (customer_id) -> c (id);
-- Views created before their tables gained NOT NULL, unique and foreign key
-- columns still serve as key join inputs.
BEGIN;
CREATE VIEW customer_rows AS SELECT * FROM customers;
CREATE VIEW order_rows AS SELECT * FROM orders;
ALTER TABLE customers ADD COLUMN email text UNIQUE, ADD COLUMN active boolean NOT NULL DEFAULT true;
ALTER TABLE orders ADD COLUMN seller_id int NOT NULL DEFAULT 1 REFERENCES customers;
SELECT FROM order_rows o JOIN customer_rows c FOR KEY o (customer_id) -> c (id);
ROLLBACK;

-- DISTINCT exactly on the key keeps every key value, once.
SELECT FROM orders o JOIN (SELECT DISTINCT id FROM customers) c
  FOR KEY o (customer_id) -> c (id);
-- GROUP BY restores the uniqueness that a fan-out lost.
SELECT FROM orders o
JOIN (SELECT c.id FROM customers c LEFT JOIN orders o2
        FOR KEY c (id) <- o2 (customer_id) GROUP BY c.id) c
  FOR KEY o (customer_id) -> c (id);
-- Extra grouping columns are fine, because the key determines them.
SELECT FROM orders o JOIN (SELECT id, name FROM customers GROUP BY id, name) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: HAVING can remove a group
SELECT FROM orders o JOIN (SELECT id FROM customers GROUP BY id HAVING id > 1) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: grouping sets output null for omitted columns
SELECT FROM (SELECT customer_id FROM orders GROUP BY GROUPING SETS ((customer_id), ())) g
JOIN customers c FOR KEY g (customer_id) -> c (id);
-- rejected, reason: a grouping set can duplicate the key
SELECT FROM orders o
JOIN (SELECT id FROM customers GROUP BY GROUPING SETS ((id), (id))) c
  FOR KEY o (customer_id) -> c (id);

-- The same key = value filter on both sides keeps the join proven.
SELECT FROM (SELECT * FROM orders WHERE customer_id = 1) o
JOIN (SELECT * FROM customers WHERE id = 1) c
  FOR KEY o (customer_id) -> c (id);
-- A cross-type comparison counts as the same filter.
SELECT FROM (SELECT * FROM orders WHERE customer_id = 1::bigint) o
JOIN (SELECT * FROM customers WHERE id = 1::bigint) c
  FOR KEY o (customer_id) -> c (id);
-- A matched filter carries through to the next key join.
SELECT FROM (SELECT * FROM shipments WHERE order_id = 10) s
JOIN (SELECT * FROM order_items WHERE order_id = 10) i
  FOR KEY s (order_id, item_no) -> i (order_id, item_no)
JOIN (SELECT * FROM orders WHERE id = 10) o
  FOR KEY i (order_id) -> o (id);
-- rejected, reason: the two filters use different values
SELECT FROM (SELECT * FROM orders WHERE customer_id = 2) o
JOIN (SELECT * FROM customers WHERE id = 1) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: only the referenced side is filtered
SELECT FROM orders o JOIN (SELECT * FROM customers WHERE id = 1) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: not a key = value filter
SELECT FROM (SELECT * FROM orders WHERE customer_id > 1) o
JOIN (SELECT * FROM customers WHERE id > 1) c
  FOR KEY o (customer_id) -> c (id);
-- rejected, reason: a volatile value can differ between the two filters
SELECT FROM (SELECT * FROM orders WHERE customer_id = random()::int) o
JOIN (SELECT * FROM customers WHERE id = random()::int) c
  FOR KEY o (customer_id) -> c (id);

-- A stored join keeps the declared form and records what its proof used.
CREATE VIEW order_customers AS
SELECT o.id, c.name FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
SELECT pg_get_viewdef('order_customers'::regclass, true);
SELECT pg_describe_object(d.refclassid, d.refobjid, d.refobjsubid) AS proof_uses
FROM pg_depend d JOIN pg_rewrite r ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid
WHERE r.ev_class = 'order_customers'::regclass AND d.deptype = 'k'
ORDER BY 1;

-- rejected, reason: each change removes evidence the view's proof uses
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey NOT ENFORCED;
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey DEFERRABLE;
ALTER TABLE customers ENABLE ROW LEVEL SECURITY;
CREATE TABLE vip_customers () INHERITS (customers);
ALTER TABLE orders DROP CONSTRAINT orders_customer_id_fkey;
ALTER TABLE orders ALTER customer_id DROP NOT NULL;

-- CASCADE drops the view instead.
BEGIN;
ALTER TABLE orders DROP CONSTRAINT orders_customer_id_fkey CASCADE;
SELECT to_regclass('order_customers');
ROLLBACK;

-- Rebuilding the key's index is not a change to the proof.
REINDEX INDEX customers_pkey;
REINDEX INDEX CONCURRENTLY customers_pkey;

-- Replacing a view rechecks the proofs that go through it, also when they
-- are nested in a CTE, in a chain of joins or over a composite key.
BEGIN;
CREATE VIEW customer_keys AS SELECT id FROM customers;
CREATE VIEW shipment_customers AS
WITH k AS (SELECT id FROM customer_keys)
SELECT s.id
FROM shipments s
JOIN order_items i FOR KEY s (order_id, item_no) -> i (order_id, item_no)
JOIN orders o FOR KEY i (order_id) -> o (id)
JOIN k FOR KEY o (customer_id) -> k (id);
CREATE OR REPLACE VIEW customer_keys AS SELECT id FROM customers;
CREATE OR REPLACE VIEW customer_keys AS SELECT id FROM customers WHERE id > 1;
ROLLBACK;

-- A replacement that is proven through another table's key moves the
-- recorded dependencies to that key.
BEGIN;
CREATE TABLE archived_customers (id int PRIMARY KEY);
INSERT INTO archived_customers SELECT id FROM customers;
ALTER TABLE orders ADD FOREIGN KEY (customer_id) REFERENCES archived_customers;
CREATE VIEW customer_keys AS SELECT id FROM customers;
CREATE VIEW order_customer_keys AS
SELECT o.id FROM orders o JOIN customer_keys c FOR KEY o (customer_id) -> c (id);
CREATE OR REPLACE VIEW customer_keys AS SELECT id FROM archived_customers;
SELECT pg_describe_object(d.refclassid, d.refobjid, d.refobjsubid) AS proof_uses
FROM pg_depend d JOIN pg_rewrite r ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid
WHERE r.ev_class = 'order_customer_keys'::regclass AND d.deptype = 'k'
ORDER BY 1;
ROLLBACK;

-- rejected, reason: the replacement proves the join with a different equality
BEGIN;
CREATE FUNCTION bucket_eq(a int, b int) RETURNS boolean IMMUTABLE STRICT
  LANGUAGE sql AS 'SELECT a / 10 = b / 10';
CREATE FUNCTION bucket_cmp(a int, b int) RETURNS int IMMUTABLE STRICT
  LANGUAGE sql AS 'SELECT sign(a / 10 - b / 10)::int';
CREATE OPERATOR =# (LEFTARG = int, RIGHTARG = int, FUNCTION = bucket_eq,
                    COMMUTATOR = =#, MERGES);
CREATE OPERATOR CLASS bucket_ops FOR TYPE int USING btree AS
  OPERATOR 3 =# (int, int), FUNCTION 1 bucket_cmp(int, int);
CREATE TABLE bucketed_customers (id int);
CREATE UNIQUE INDEX ON bucketed_customers USING btree (id bucket_ops);
INSERT INTO bucketed_customers VALUES (1), (11);
ALTER TABLE orders ADD FOREIGN KEY (customer_id) REFERENCES bucketed_customers (id);
CREATE VIEW customer_keys AS SELECT id FROM customers;
CREATE VIEW order_customer_keys AS
SELECT o.id FROM orders o JOIN customer_keys c FOR KEY o (customer_id) -> c (id);
CREATE OR REPLACE VIEW customer_keys AS SELECT id FROM bucketed_customers;
ROLLBACK;

-- A function in a matched filter must not become volatile, by either ALTER
-- or CREATE OR REPLACE.
CREATE FUNCTION first_customer() RETURNS int IMMUTABLE LANGUAGE sql AS 'SELECT 1';
CREATE VIEW first_customer_orders AS
SELECT o.id
FROM (SELECT * FROM orders WHERE customer_id = first_customer()) o
JOIN (SELECT * FROM customers WHERE id = first_customer()) c
  FOR KEY o (customer_id) -> c (id);
ALTER FUNCTION first_customer() VOLATILE;
CREATE OR REPLACE FUNCTION first_customer() RETURNS int VOLATILE LANGUAGE sql AS 'SELECT 1';
ALTER FUNCTION first_customer() STABLE;
-- After its tables gain columns, the view is still rechecked against the
-- columns it was created with.
BEGIN;
ALTER TABLE customers ADD COLUMN email text UNIQUE;
ALTER TABLE orders ADD COLUMN coupon int NOT NULL DEFAULT 0;
ALTER FUNCTION first_customer() IMMUTABLE;
ALTER FUNCTION first_customer() VOLATILE;
ROLLBACK;
DROP VIEW first_customer_orders;
DROP FUNCTION first_customer();

-- Materialized views are checked the same way.  Once no stored proof is
-- left, the change goes through.
CREATE MATERIALIZED VIEW order_customers_mv AS
SELECT o.id, c.name FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey NOT ENFORCED;
DROP MATERIALIZED VIEW order_customers_mv;
DROP VIEW order_customers;
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey NOT ENFORCED;
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey ENFORCED;

-- A rule records the proofs in its action and in its condition.  Firing the
-- rule renumbers the tables of the proof in its action.
BEGIN;
CREATE TABLE audit (id int);
CREATE RULE audit_orders AS ON INSERT TO audit
  DO ALSO SELECT c.id FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id) WHERE o.id = NEW.id;
INSERT INTO audit VALUES (10);
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey DEFERRABLE;
ROLLBACK;
BEGIN;
CREATE TABLE audit (id int);
CREATE RULE audit_orders AS ON INSERT TO audit
  WHERE EXISTS (SELECT FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id)) DO ALSO NOTHING;
ALTER TABLE orders ALTER CONSTRAINT orders_customer_id_fkey DEFERRABLE;
ROLLBACK;

-- A proof over ONLY customers assumes nothing about its children, so a child
-- scanned elsewhere in the statement does not invalidate it.
BEGIN;
CREATE TABLE vip_customers () INHERITS (customers);
PREPARE only_order_names AS
SELECT FROM orders o JOIN ONLY customers c FOR KEY o (customer_id) -> c (id)
WHERE EXISTS (SELECT FROM customers);
EXECUTE only_order_names;
EXECUTE only_order_names;
ROLLBACK;
DEALLOCATE only_order_names;

-- A view stores the table a qualified endpoint selects, not its spelling.
BEGIN;
CREATE VIEW qualified_order_customers AS
SELECT count(*) FROM public.orders JOIN customers
  FOR KEY public.orders (customer_id) -> customers (id);
SELECT pg_get_viewdef('qualified_order_customers'::regclass, true);
ROLLBACK;

-- A view whose endpoint is a join alias shows the new name of a renamed key
-- column.
BEGIN;
CREATE VIEW referred_orders AS
SELECT r.name
FROM (orders o JOIN customers c FOR KEY o (customer_id) -> c (id)) AS j(order_id)
LEFT JOIN customers r FOR KEY j (referrer_id) -> r (id);
ALTER TABLE orders RENAME COLUMN referrer_id TO referred_by;
SELECT pg_get_viewdef('referred_orders'::regclass, true);
ROLLBACK;

-- Left in place for the dump and upgrade tests.
CREATE VIEW order_customers AS
SELECT o.id, c.name FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
