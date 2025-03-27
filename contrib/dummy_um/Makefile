# Makefile for the dummy_um PostgreSQL extension

MODULES = dummy_um
EXTENSION = dummy_um
DATA = dummy_um--1.0.sql
PG_CONFIG = pg_config
PGXS = $(shell $(PG_CONFIG) --pgxs)
REGRESS = basic
include $(PGXS)

# Additional build instructions can be added here if necessary.