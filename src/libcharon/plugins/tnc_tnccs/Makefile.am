
INCLUDES = \
	-I$(top_srcdir)/src/libstrongswan \
	-I$(top_srcdir)/src/libtncif \
	-I$(top_srcdir)/src/libtnccs

AM_CFLAGS = -rdynamic

if MONOLITHIC
noinst_LTLIBRARIES = libstrongswan-tnc-tnccs.la
else
plugin_LTLIBRARIES = libstrongswan-tnc-tnccs.la
libstrongswan_tnc_tnccs_la_LIBADD = \
	$(top_builddir)/src/libtncif/libtncif.la \
	$(top_builddir)/src/libtnccs/libtnccs.la
endif

libstrongswan_tnc_tnccs_la_SOURCES = \
	tnc_tnccs_plugin.h tnc_tnccs_plugin.c \
	tnc_tnccs_manager.h tnc_tnccs_manager.c

libstrongswan_tnc_tnccs_la_LDFLAGS = -module -avoid-version
