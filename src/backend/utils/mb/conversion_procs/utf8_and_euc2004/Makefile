#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_euc2004/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_euc2004
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_srcdir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_euc2004
PGFILEDESC	= "utf8 <-> euc2004 text conversions"
MAPFILES	= $(mapfiledir)/euc_jis_2004_to_utf8.map \
			  $(mapfiledir)/utf8_to_euc_jis_2004.map

include $(srcdir)/../proc.mk
