# contrib/test_buffer/Makefile

MODULE_big	= test_bufmgr
OBJS		= test_bufmgr.o

EXTENSION = test_bufmgr
DATA = test_bufmgr--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/test_bufmgr
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
