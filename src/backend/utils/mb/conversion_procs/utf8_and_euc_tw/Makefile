#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_euc_tw/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_euc_tw
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_srcdir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_euc_tw
PGFILEDESC	= "utf8 <-> euc_tw text conversions"
MAPFILES	= $(mapfiledir)/euc_tw_to_utf8.map \
			  $(mapfiledir)/utf8_to_euc_tw.map

include $(srcdir)/../proc.mk
