# To let coverage gaps inform which files to translate, I used bpftrace to get
# the call stacks that reach a pg_mblen* call w/ retval=1 but reach no such
# call with retval!=1.

cd src/test/regress
# fairly clean
tests='text strings regex'
# much noisier
tests="$tests arrays collate.icu.utf8 tsdicts tsearch tsrf tstypes"
# expects v15+; v14 lacks test_setup
runnable=test_setup
for t in $tests; do
  perl ../../../ascii2utf8sql.pl <sql/$t.sql >sql/$t.utf8.sql
  # TODO script not ready for expected outputs: deletes more than it should
  perl ../../..//ascii2utf8sql.pl <expected/$t.out >expected/$t.utf8.out
  runnable="$runnable $t.utf8"
done
cd ../../..
make installcheck-tests TESTS="$runnable tablespace" "$@"
echo '==== all added errors'
echo '==== (includes false positives from flawed munging of expected output)'
grep '^[+]ERROR' src/test/regress/regression.diffs | sort | uniq -c
echo '==== all examples of goal error: invalid byte sequence for encoding'
grep 'invalid byte sequence for encoding' src/test/regress/regression.diffs
