#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_johab/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_johab
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_johab
PGFILEDESC	= "utf8 <-> johab text conversions"
MAPFILES	= $(mapfiledir)/johab_to_utf8.map \
			  $(mapfiledir)/utf8_to_johab.map

include $(srcdir)/../proc.mk
