# -*- makefile -*-
#
# Copyright (c) 2024      Switch Barrier Accelerator Implementation
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

sources = \
        coll_switch_barrier.h \
        coll_switch_barrier_component.c \
        coll_switch_barrier_module.c \
        coll_switch_barrier_control_plane.c \
        coll_switch_barrier_iommu.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_ompi_coll_switch_barrier_DSO
component_noinst =
component_install = mca_coll_switch_barrier.la
else
component_noinst = libmca_coll_switch_barrier.la
component_install =
endif

mcacomponentdir = $(ompilibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_coll_switch_barrier_la_SOURCES = $(sources)
mca_coll_switch_barrier_la_LDFLAGS = -module -avoid-version
mca_coll_switch_barrier_la_LIBADD = $(top_builddir)/ompi/lib@OMPI_LIBMPI_NAME@.la

noinst_LTLIBRARIES = $(component_noinst)
libmca_coll_switch_barrier_la_SOURCES = $(sources)
libmca_coll_switch_barrier_la_LDFLAGS = -module -avoid-version
