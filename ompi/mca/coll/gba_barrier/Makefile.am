# -*- Makefile -*-
#
# Copyright (c) 2024      Global Barrier Accelerator Implementation
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Make the library
noinst_LTLIBRARIES = libmca_coll_gba_barrier.la

# Source files
libmca_coll_gba_barrier_la_SOURCES = \
    coll_gba_barrier.h \
    coll_gba_barrier_component.c \
    coll_gba_barrier_module.c \
    coll_gba_barrier_control.c

# Conditionally install the header file
ompidir = $(ompiincludedir)/ompi/mca/coll/gba_barrier
ompi_HEADERS = coll_gba_barrier.h
