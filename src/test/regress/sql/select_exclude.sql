--
-- SELECT_EXCLUDE
--

SET client_min_messages TO 'warning';

DROP TABLE IF EXISTS users;
DROP TABLE IF EXISTS orders;

RESET client_min_messages;

CREATE TABLE users (
    id          INT PRIMARY KEY,
    email       TEXT,
    name        TEXT,
    created_at  TIMESTAMP,
    updated_at  TIMESTAMP
);

CREATE TABLE orders (
    id          INT PRIMARY KEY,
    user_id     INT REFERENCES users(id),
    amount      NUMERIC(10,2),
    status      TEXT,
    created_at  TIMESTAMP
);

-- Insert sample data
INSERT INTO users (id, email, name, created_at, updated_at) VALUES
(1,'alice@example.com','Alice','2026-01-01 10:00:00','2026-01-05 09:00:00'),
(2,'bob@example.com','Bob','2026-01-02 11:00:00','2026-01-06 10:00:00'),
(3,'carol@example.com','Carol','2026-01-03 12:00:00',NULL),
(4,NULL,'Dave','2026-01-04 13:00:00','2026-01-07 11:00:00');

INSERT INTO orders (id, user_id, amount, status, created_at) VALUES
(101,1,50.00,'paid','2026-02-01 09:00:00'),
(102,1,75.50,'shipped','2026-02-02 10:00:00'),
(103,2,20.00,'cancelled','2026-02-03 11:00:00'),
(104,3,100.00,'paid','2026-02-04 12:00:00');

-- Basic SELECT with EXCLUDE condition
-- Single column
SELECT * EXCLUDE (updated_at)
FROM users
ORDER BY id;

-- Multiple columns
SELECT * EXCLUDE (email, created_at)
FROM users
ORDER BY id;

-- Exclude all but one column
SELECT * EXCLUDE (email, name, created_at, updated_at)
FROM users
ORDER BY id;

-- Exclude all columns
SELECT * EXCLUDE (id, email, name, created_at, updated_at)
FROM users;

-- EXCLUDE all using exclude list but overall SELECT list is not empty
SELECT id, users.* EXCLUDE (id, email, name, created_at, updated_at)
FROM users;

-- Aliasing with EXCLUDE
SELECT * EXCLUDE (u.email)
FROM users AS u
ORDER BY u.id;

-- Expressions with EXCLUDE
SELECT * EXCLUDE (updated_at), 1 + 1 AS two
FROM users
ORDER BY id;

-- JOINs with EXCLUDE
-- Join, unqualified EXCLUDE
SELECT * EXCLUDE (created_at)
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, qualified EXCLUDE, one table
SELECT * EXCLUDE (users.created_at)
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, qualified EXCLUDE, both tables
SELECT * EXCLUDE (users.created_at, orders.amount)
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Join, aliased tables with EXCLUDE
SELECT * EXCLUDE (u.created_at, o.amount)
FROM users AS u
JOIN orders AS o ON o.user_id = u.id
ORDER BY u.id, o.id;

-- Qualified stars
SELECT users.* EXCLUDE (email), orders.* EXCLUDE (user_id)
FROM users
LEFT JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Name collision
SELECT * EXCLUDE (id)
FROM users
JOIN orders ON orders.user_id = users.id
ORDER BY users.id, orders.id;

-- Subqueries with EXCLUDE
SELECT * EXCLUDE (u.created_at)
FROM (
    SELECT * FROM users
) u
ORDER BY id;

-- CTEs with EXCLUDE
WITH base_users AS (
    SELECT * FROM users
)
SELECT * EXCLUDE (base_users.updated_at)
FROM base_users
ORDER BY id;

-- WHERE clause with EXCLUDE
SELECT * EXCLUDE (email)
FROM users
WHERE email IS NOT NULL
ORDER BY created_at;

-- DISTINCT with EXCLUDE
SELECT DISTINCT * EXCLUDE (updated_at)
FROM users;

-- Multiple stars with EXCLUDE
SELECT
*,
users.* EXCLUDE (id),
orders.* EXCLUDE (user_id)
FROM users
LEFT JOIN orders ON orders.user_id = users.id;

-- CROSS JOIN with EXCLUDE
SELECT * EXCLUDE (email, status) FROM users, orders ORDER BY users.id, orders.id;
SELECT * EXCLUDE (email, status) FROM users CROSS JOIN orders ORDER BY users.id, orders.id;

-- Error cases
-- Non-existent column in EXCLUDE list (error case)
SELECT * EXCLUDE (does_not_exist)
FROM users;

-- Empty EXCLUDE list (error case)
SELECT * EXCLUDE ()
FROM users;

-- Exclude without star (error case)
SELECT id, email EXCLUDE (email)
FROM users;

-- Exclude with duplicate column names (error case)
SELECT * EXCLUDE (id, id)
FROM users;

-- clean up
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS users;
