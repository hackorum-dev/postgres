#
# Makefile for storage/ipc
#
# src/backend/storage/ipc/Makefile
#

subdir = src/backend/storage/ipc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	barrier.o \
	dsm.o \
	dsm_impl.o \
	dsm_registry.o \
	ipc.o \
	ipci.o \
	latch.o \
	pmsignal.o \
	procarray.o \
	procsignal.o \
	shm_mq.o \
	shm_toc.o \
	shmem.o \
	signalfuncs.o \
	sinval.o \
	sinvaladt.o \
	standby.o

ifeq ($(PORTNAME), win32)
OBJS += dsm_impl_windows.o
else
# TODO: Only build the POSIX and System V implementatiions on
# platforms where they are available. Currently there are large #ifdef
# blocks in the source files, but would be nicer to skip compiling
# them altogether.
OBJS += dsm_impl_posix.o dsm_impl_sysv.o dsm_impl_mmap.o
endif

include $(top_srcdir)/src/backend/common.mk
