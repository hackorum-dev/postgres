#!/bin/sh
sort - > tmp1
./catalog_oidjoins.pl | sort > tmp2
diff -C 0 tmp1 tmp2 | grep -E '^[+-] '
rm tmp1 tmp2
