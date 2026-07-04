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

-- rejected, reason: nothing would recheck a stored join when its constraints
-- change
BEGIN;
CREATE VIEW stored_view AS SELECT o.id FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;
BEGIN;
CREATE MATERIALIZED VIEW stored_matview AS SELECT o.id FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;
BEGIN;
CREATE TABLE audit (id int);
CREATE RULE stored_rule AS ON INSERT TO audit DO ALSO SELECT o.id FROM orders o JOIN customers c FOR KEY o (customer_id) -> c (id);
ROLLBACK;
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
