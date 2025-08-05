#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_gbk/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_gbk
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_gbk
PGFILEDESC	= "utf8 <-> gbk text conversions"
MAPFILES	= $(mapfiledir)/gbk_to_utf8.map \
			  $(mapfiledir)/utf8_to_gbk.map

include $(srcdir)/../proc.mk
