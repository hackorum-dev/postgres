#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_cyrillic/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_cyrillic
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_builddir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_cyrillic
PGFILEDESC	= "utf8 <-> cyrillic text conversions"
MAPFILES	= $(mapfiledir)/utf8_to_koi8r.map \
			  $(mapfiledir)/koi8r_to_utf8.map \
			  $(mapfiledir)/utf8_to_koi8u.map \
			  $(mapfiledir)/koi8u_to_utf8.map

include $(srcdir)/../proc.mk
