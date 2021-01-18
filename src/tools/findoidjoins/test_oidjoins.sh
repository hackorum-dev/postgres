#!/bin/sh
echo "README:"
tail -n+60 README | grep -E '^Join' | ./diff_oidjoins.sh \
  | grep -v -f bogus_oidjoins.txt | tee diff1

echo "findoidjoins:"
./findoidjoins regression | ./diff_oidjoins.sh \
  | grep -v -f bogus_oidjoins.txt | tee diff2

echo "diff of diffs:"
diff diff1 diff2

rm diff1 diff2
