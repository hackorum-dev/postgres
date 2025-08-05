#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_uhc/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_uhc
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_srcdir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_uhc
PGFILEDESC	= "utf8 <-> uhc text conversions"
MAPFILES	= $(mapfiledir)/uhc_to_utf8.map \
			  $(mapfiledir)/utf8_to_uhc.map

include $(srcdir)/../proc.mk
