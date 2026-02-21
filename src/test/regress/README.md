
# Running tests

Documentation concerning how to run these regression tests and interpret
the results can be found in the PostgreSQL manual, in the chapter
"Regression Tests".


# Writing tests

Regression tests in PostgreSQL are used to automatically verify that the
database server produces the expected results for a wide variety of SQL
operations and features. Writing good regression tests helps ensure that new
changes do not break existing functionality.

### 1. Locate or Create SQL and Expected Files

- All regression test SQL files are located in `src/test/regress/sql/`.
- For each `.sql` test file, there should be a corresponding expected output
file in `src/test/regress/expected/` with a `.out` extension.

### 2. Write a Test

Each test comprises a `.sql` file in `src/test/regress/sql/`, and a `.out` file
in `src/test/regress/expected`.

The `.sql` file executes commands that exercise the functionality you want to
test. Include queries, DDL, DML, and edge cases as appropriate. Add comments in
your SQL file describing what and why you are testing.

Example (`mytest.sql`):
```sql
-- Test basic insert and select
CREATE TABLE mydemo(a int, b text);
INSERT INTO mydemo VALUES (1, 'foo'), (2, 'bar');
SELECT * FROM mydemo ORDER BY a;
```

Create a `.out` file in `src/tst/regress/expected` that includes both the
queries of the `.sql` along with the desired output. Try to make test outputs
stable: avoid things that vary between runs (timestamps, OIDs, etc.) unless
necessary. Use pattern matching (`--@@IGNORE ...@@` directives) in the output
if needed for platform-specific or variable content (see below for details) and
document what you are testing and why. Finally, include both your `.sql` and
`.out`, along with any schedule changes.

Edit `src/test/regress/parallel_schedule` or `serial_schedule` to add your test
file. List the base name (without .sql) in the appropriate place, depending on
whether the test can run in parallel.
Run all tests with `make check` in the `src/test/regress` directory.

---

By following these steps for each new feature or bug fix, you will help keep
PostgreSQL reliable and trustworthy for all users.


# Pattern Matching for Regression Test Outputs

This document specifies pattern matching options for comparing regression test
outputs against expected results. These options help tests pass across
different configurations (e.g., block sizes) where exact output may vary.

## Syntax

Options are specified using SQL comment directives:

```sql
--@@IGNORE option1, option2@@
```

To disable options within a section:

```sql
--@@CHECK option1@@
```

Options remain active until changed by another directive or end of file.


- Directives are processed line-by-line during comparison
- `CHECK [options]` re-enables strict matching for specified options
- `CHECK ALL` enables everything
- `CHECK DEFAULT` ignore space and case
- Unknown options generate a warning but don't fail the test
- Options are case-insensitive (`IGNORE` = `ignore` = `Ignore`)



## Options

### `case` - Ignore Case Differences

Treats uppercase and lowercase letters as equivalent.

**Pattern: case**
```
--@@IGNORE: case@@
SELECT * FROM users;
```

**Accepts:**
```
SELECT * FROM users;
```

**Accepts:**
```
select * from users;
```

**Accepts:**
```
SELECT * FROM USERS;
```

**Rejects:**
```
SELECT * FROM accounts;
```

---

### `comments` - Ignore SQL Comments

Ignores SQL comments (`--` and `/* */`) when comparing.

**Pattern: comments**
```
--@@IGNORE: comments@@
SELECT id FROM users;
```

**Accepts:**
```
SELECT id FROM users;
```

**Accepts:**
```
SELECT id FROM users; -- fetch all ids
```

**Accepts:**
```
/* query */ SELECT id FROM users;
```

**Rejects:**
```
SELECT name FROM users;
```

---

### `spaces` - Ignore Whitespace Differences

Treats any whitespace sequence as equivalent (spaces, tabs, multiple spaces).

**Pattern: spaces**
```
--@@IGNORE: spaces@@
Seq Scan on foo  (cost=0.00..1.00)
```

**Accepts:**
```
Seq Scan on foo (cost=0.00..1.00)
```

**Accepts:**
```
Seq  Scan  on  foo   (cost=0.00..1.00)
```

**Rejects:**
```
SeqScan on foo (cost=0.00..1.00)
```

---

### `numbers` - Ignore Numeric Value Differences

Treats all numeric values (integers, decimals) as wildcards.

**Pattern: numbers**
```
--@@IGNORE: numbers@@
Buffers: shared hit=10, read=5
(cost=0.00..123.45 rows=1000 width=8)
```

**Accepts:**
```
Buffers: shared hit=10, read=5
(cost=0.00..123.45 rows=1000 width=8)
```

**Accepts:**
```
Buffers: shared hit=42, read=17
(cost=0.00..999.99 rows=5000 width=16)
```

**Rejects:**
```
Buffers: shared hit=10
(cost=0.00..123.45 rows=1000 width=8)
```

---

### `result-lines` - Ignore Extra Lines in Result

Allows the actual result to contain lines not present in expected output.

**Pattern: result-lines**
```
--@@IGNORE: result-lines@@
BEGIN
COMMIT
```

**Accepts:**
```
BEGIN
COMMIT
```

**Accepts:**
```
BEGIN
INSERT 0 1
COMMIT
```

**Accepts:**
```
BEGIN
INSERT 0 1
UPDATE 5
COMMIT
```

**Rejects:**
```
BEGIN
ROLLBACK
```

---

### `expected-lines` - Ignore Expected Lines Missing from Result

Allows the actual result to omit lines present in expected output.

**Pattern: expected-lines**
```
--@@IGNORE: expected-lines@@
line one
line two
line three
```

**Accepts:**
```
line one
line two
line three
```

**Accepts:**
```
line one
line three
```

**Accepts:**
```
line two
```

**Rejects:**
```
line one
line four
line three
```

---

## Combining Options

Multiple options can be combined:

**Pattern: Combined options**
```
--@@IGNORE: case, spaces, numbers@@
Seq Scan on FOO (cost=0.00..100.00 rows=1000)
```

**Accepts:**
```
seq scan on foo  (cost=0.00..50.00 rows=500)
```

**Accepts:**
```
SEQ SCAN ON FOO (cost=0.00..999.99 rows=9999)
```

**Rejects:**
```
Index Scan on foo (cost=0.00..100.00 rows=1000)
```

---

## Toggling Options

Options can be enabled and disabled within a file:

```sql
-- Strict matching by default
SELECT 1;
 ?column?
----------
        1
(1 row)

--@@IGNORE: numbers@@
-- Numbers can vary in this section
EXPLAIN SELECT * FROM foo;
                      QUERY PLAN
------------------------------------------------------
 Seq Scan on foo  (cost=0.00..1.00 rows=100 width=32)
(1 row)

--@@CHECK: numbers@@
-- Back to strict matching
SELECT 2;
 ?column?
----------
        2
(1 row)
```



**Pattern: toggling options**
```
--@@IGNORE: numbers@@
cost=0.00..10.00
--@@IGNORE: spaces@@
col1   col2
--@@CHECK: numbers@@
rows=50
--@@IGNORE: case@@
Select Done
```

**Accepts:**
```
cost=0.00..99.99
col1 col2
rows=50
select done
```

**Accepts:**
```
cost=123.45..999.99
col1      col2
rows=50
SELECT DONE
```

**Rejects:**
```
cost=0.00..99.99
col1 col2
rows=999
select done
```

**Rejects:**
```
const=0.00 .. 99.99
col1  col2
rows=50
Select Done
```

**Rejects:**
```
const=0.00..99.99
col2  col1
rows=50
Select Done
```
