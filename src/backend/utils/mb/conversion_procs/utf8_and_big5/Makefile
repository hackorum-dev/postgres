#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_big5/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_big5
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_big5
PGFILEDESC	= "utf8 <-> big5 text conversions"
MAPFILES	= $(mapfiledir)/big5_to_utf8.map \
			  $(mapfiledir)/utf8_to_big5.map

include $(srcdir)/../proc.mk
