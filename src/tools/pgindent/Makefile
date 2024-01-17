#-------------------------------------------------------------------------
#
# src/tools/pgindent/Makefile
#
# Copyright (c) 2024, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

subdir = src/tools/pgindent
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

clean distclean:
	rm -rf log/ tmp_check/

# Provide this alternate test name to allow testing pgindent without building
# all of the surrounding Postgres installation.
.PHONY: test

pg_bsd_indent:
	$(MAKE) -C ../pg_bsd_indent pg_bsd_indent

test: pg_bsd_indent
	$(prove_installcheck)
