#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_gb18030/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_gb18030
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_gb18030
PGFILEDESC	= "utf8 <-> gb18030 text conversions"
MAPFILES	= $(mapfiledir)/gb18030_to_utf8.map \
			  $(mapfiledir)/utf8_to_gb18030.map

include $(srcdir)/../proc.mk
