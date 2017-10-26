#!/usr/bin/bash

echo "../../preproc/ecpg --regression --enable-utext -I./../../include -I. -o utext.c utext.pgc"

#../../preproc/ecpg --regression --enable-utext -I./../../include -I. -o utext.c utext.pgc


../../preproc/ecpg --regression -r no_indicator --enable-utext -I./../../include -I. -o utext.c utext.pgc

gcc  -Wall -Wmissing-prototypes -Wpointer-arith -Wdeclaration-after-statement -Wendif-labels -Wmissing-format-attribute -Wformat-security -fno-strict-aliasing -fwrapv -fexcess-precision=standard -O0 -pthread -D_REENTRANT -D_THREAD_SAFE -D_POSIX_PTHREAD_SEMANTICS -I../../include -I../../../../../src/interfaces/ecpg/include -I../../../../../src/interfaces/libpq -I../../../../../src/include -DLINUX_OOM_SCORE_ADJ=0 -DLINUX_OOM_ADJ=0 -D_GNU_SOURCE -I/db/pgmaster/run/include -g  -c -o utext.o utext.c


gcc  -Wall -Wmissing-prototypes -Wpointer-arith -Wdeclaration-after-statement -Wendif-labels -Wmissing-format-attribute -Wformat-security -fno-strict-aliasing -fwrapv -fexcess-precision=standard -O0 -pthread -D_REENTRANT -D_THREAD_SAFE -D_POSIX_PTHREAD_SEMANTICS utext.o -L../../ecpglib -L../../pgtypeslib -L../../../../../src/interfaces/libpq -L../../../../../src/port -L../../../../../src/common -L/db/pgmaster/run/lib  -Wl,--as-needed -Wl,-rpath,--enable-new-dtags  -lecpg -lpgtypes -lpq -lpgcommon -lpgport -lselinux -lxslt -lxml2 -lpam -lssl -lcrypto -lgssapi_krb5 -lz -lrt -lcrypt -ldl -lm  -g -o utext


./utext >results/utext.ret 2>&1
