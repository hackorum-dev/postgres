#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_euc_kr/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_euc_kr
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_euc_kr
PGFILEDESC	= "utf8 <-> euc_kr text conversions"
MAPFILES	= $(mapfiledir)/euc_kr_to_utf8.map \
			  $(mapfiledir)/utf8_to_euc_kr.map

include $(srcdir)/../proc.mk
