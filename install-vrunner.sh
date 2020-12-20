#!/bin/bash
set -e

SRCDIR=$1
PGPREFIX=$SRCDIR/tmp_install/usr/local/pgsql

cd $PGPREFIX/bin
cp $SRCDIR/src/tools/valgrind.supp ./

echo ""
cat << 'EOF' > vrunner
#!/bin/bash
set -e
BD="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SN="$(basename "${BASH_SOURCE[0]}")"
OBIN="_$SN"
[[ $LD_PRELOAD = *"valgrind"* ]] && exec $BD/$OBIN "$@"

exec valgrind --quiet  --exit-on-first-error=yes --error-exitcode=1 --leak-check=no --time-stamp=yes \
 --gen-suppressions=all --suppressions=$BD/valgrind.supp \
 --trace-children=yes --trace-children-skip="/bin/*,/usr/bin/*" \
 $BD/$OBIN "$@"
EOF
chmod a+x vrunner

for f in *; do
    if [ "$f" = "vrunner" ] || [ ! -x "$f" ]; then continue; fi
    mv "$f" "_$f"
    ln -s vrunner "$f"
    chmod a+x "$f"
done
