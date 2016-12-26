# Makefile for power/arm plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = power_arm.la
power_arm_la_SOURCES = power_arm.c
power_arm_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
power_arm_la_LIBADD = ../common/libpower_common.la -lm

force:
$(power_arm_la_LIBADD) : force
	@cd `dirname $@` && $(MAKE) `basename $@`
