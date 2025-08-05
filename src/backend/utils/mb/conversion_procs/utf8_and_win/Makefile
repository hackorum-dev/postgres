#-------------------------------------------------------------------------
#
# src/backend/utils/mb/conversion_procs/utf8_and_win/Makefile
#
#-------------------------------------------------------------------------
subdir = src/backend/utils/mb/conversion_procs/utf8_and_win
top_builddir = ../../../../../..
include $(top_builddir)/src/Makefile.global
mapfiledir = $(top_srcdir)/src/backend/utils/mb/Unicode

NAME		= utf8_and_win
PGFILEDESC	= "utf8 <-> win text conversions"
MAPFILES	= $(mapfiledir)/utf8_to_win1250.map \
			  $(mapfiledir)/utf8_to_win1251.map \
			  $(mapfiledir)/utf8_to_win1252.map \
			  $(mapfiledir)/utf8_to_win1253.map \
			  $(mapfiledir)/utf8_to_win1254.map \
			  $(mapfiledir)/utf8_to_win1255.map \
			  $(mapfiledir)/utf8_to_win1256.map \
			  $(mapfiledir)/utf8_to_win1257.map \
			  $(mapfiledir)/utf8_to_win1258.map \
			  $(mapfiledir)/utf8_to_win866.map \
			  $(mapfiledir)/utf8_to_win874.map \
			  $(mapfiledir)/win1250_to_utf8.map \
			  $(mapfiledir)/win1251_to_utf8.map \
			  $(mapfiledir)/win1252_to_utf8.map \
			  $(mapfiledir)/win1253_to_utf8.map \
			  $(mapfiledir)/win1254_to_utf8.map \
			  $(mapfiledir)/win1255_to_utf8.map \
			  $(mapfiledir)/win1256_to_utf8.map \
			  $(mapfiledir)/win1257_to_utf8.map \
			  $(mapfiledir)/win866_to_utf8.map \
			  $(mapfiledir)/win874_to_utf8.map \
			  $(mapfiledir)/win1258_to_utf8.map

include $(srcdir)/../proc.mk
