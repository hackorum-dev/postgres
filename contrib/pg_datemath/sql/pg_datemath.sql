--
-- Test cases for pg_datemath extension
-- Tests datediff function with various dateparts and edge cases
--

CREATE EXTENSION pg_datemath;

--
-- Basic Day Calculations
--
SELECT datediff('day', '2024-01-01'::date, '2024-01-15'::date);
SELECT datediff('day', '2024-01-15'::date, '2024-01-01'::date);

--
-- Week Calculations
--
SELECT datediff('week', '2024-01-01'::date, '2024-01-08'::date);
SELECT datediff('week', '2024-01-01'::date, '2024-01-10'::date);

--
-- Month Calculations
--
SELECT datediff('month', '2024-01-15'::date, '2024-02-15'::date);
SELECT datediff('month', '2024-01-15'::date, '2024-02-20'::date);
SELECT datediff('month', '2024-01-31'::date, '2024-02-29'::date);

--
-- Quarter Calculations
--
SELECT datediff('quarter', '2024-01-01'::date, '2024-04-01'::date);
SELECT datediff('quarter', '2024-01-15'::date, '2024-05-20'::date);

--
-- Year Calculations
--
SELECT datediff('year', '2024-03-15'::date, '2025-03-15'::date);
SELECT datediff('year', '2024-01-01'::date, '2024-07-01'::date);

--
-- NULL Handling - STRICT functions return NULL for NULL inputs
--
SELECT datediff('day', NULL::date, '2024-01-15'::date);
SELECT datediff('day', '2024-01-01'::date, NULL::date);

--
-- Invalid Datepart
--
SELECT datediff('hour', '2024-01-01'::date, '2024-01-02'::date);

--
-- Case Insensitivity
--
SELECT datediff('MONTH', '2024-01-01'::date, '2024-02-01'::date);
SELECT datediff('Month', '2024-01-01'::date, '2024-02-01'::date);
SELECT datediff('month', '2024-01-01'::date, '2024-02-01'::date);

--
-- Edge Cases
--
SELECT datediff('day', '2024-01-01'::date, '2024-01-01'::date);
SELECT datediff('day', '2024-02-28'::date, '2024-03-01'::date);
SELECT datediff('day', '2023-02-28'::date, '2023-03-01'::date);
SELECT datediff('year', '2024-12-31'::date, '2025-01-01'::date);
SELECT datediff('year', '2020-01-01'::date, '2025-01-01'::date);
SELECT datediff('day', '1999-12-31'::date, '2000-01-01'::date);

--
-- Alias Tests
--
SELECT datediff('yy', '2024-01-01'::date, '2025-01-01'::date);
SELECT datediff('yyyy', '2024-01-01'::date, '2025-01-01'::date);
SELECT datediff('mm', '2024-01-15'::date, '2024-02-15'::date);
SELECT datediff('qq', '2024-01-01'::date, '2024-04-01'::date);
SELECT datediff('wk', '2024-01-01'::date, '2024-01-08'::date);
SELECT datediff('dd', '2024-01-01'::date, '2024-01-15'::date);

--
-- Timestamp Tests
--
SELECT datediff('day', '2024-01-01 10:30:00'::timestamp, '2024-01-15 14:45:00'::timestamp);
SELECT datediff('month', '2024-01-15 08:00:00'::timestamp, '2024-02-20 16:00:00'::timestamp);

--
-- Timestamptz Tests
--
SELECT datediff('day', '2024-01-01 10:30:00+00'::timestamptz, '2024-01-15 14:45:00+00'::timestamptz);

--
-- Additional Month Calculation Tests
--
SELECT datediff('month', '2024-01-25'::date, '2024-03-10'::date);
SELECT datediff('month', '2024-01-15'::date, '2024-02-20'::date);

--
-- Additional Quarter Calculation Tests
--
SELECT datediff('quarter', '2024-01-15'::date, '2024-05-20'::date);

--
-- Additional Year Calculation Tests
--
SELECT datediff('year', '2024-03-15'::date, '2025-06-20'::date);
SELECT datediff('year', '2020-03-15'::date, '2025-03-15'::date);
SELECT datediff('year', '2024-01-01'::date, '2024-07-01'::date);

--
-- Week Calculation Additional Tests
--
SELECT datediff('week', '2024-01-01'::date, '2024-01-15'::date);
SELECT datediff('week', '2024-01-01'::date, '2024-01-10'::date);

DROP EXTENSION pg_datemath;
