CREATE EXTENSION test_listsort;

SELECT test_listsort(10000, 500, 50, false);
SELECT test_listsort(10000, 500, 50, true);
