#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_euc_cn/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_euc_cn
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_euc_cn
PGFILEDESC	= "utf8 <-> euc_cn text conversions"
MAPFILES	= $(mapfiledir)/euc_cn_to_utf8.map \
			  $(mapfiledir)/utf8_to_euc_cn.map

include $(srcdir)/../proc.mk
