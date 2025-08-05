#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_sjis/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_sjis
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_srcdir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_sjis
PGFILEDESC	= "utf8 <-> sjis text conversions"
MAPFILES	= $(mapfiledir)/sjis_to_utf8.map \
			  $(mapfiledir)/utf8_to_sjis.map

include $(srcdir)/../proc.mk
