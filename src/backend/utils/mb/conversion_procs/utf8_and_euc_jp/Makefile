#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_euc_jp/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_euc_jp
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_euc_jp
PGFILEDESC	= "utf8 <-> euc_jp text conversions"
MAPFILES	= $(mapfiledir)/euc_jp_to_utf8.map \
			  $(mapfiledir)/utf8_to_euc_jp.map

include $(srcdir)/../proc.mk
