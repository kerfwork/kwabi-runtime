# Build the firewall probes as an ordinary PostgreSQL extension.
#
# These are diagnostics, not shipped code. They exist to answer by measurement
# the questions the firewall design depends on.

PG ?= 18

PG_CONFIG_pg16 := /opt/homebrew/opt/postgresql@16/bin/pg_config
PG_CONFIG_pg17 := /opt/homebrew/opt/postgresql@17/bin/pg_config
PG_CONFIG_pg18 := pg_config

PG_CONFIG := $(PG_CONFIG_pg$(PG))
PGBIN     := $(shell $(PG_CONFIG) --bindir)
PGINC     := $(shell $(PG_CONFIG) --includedir-server)
PKGLIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
PSQL      := $(PGBIN)/psql

PGPORT_pg16 := 5434
PGPORT_pg17 := 5433
PGPORT_pg18 := 5432
PGPORT      := $(PGPORT_pg$(PG))

MODULE := fwprobe.dylib
SRCS   := fwprobe.c fwprobe2.c

.PHONY: all build install probe probe2 uninstall clean

all: build

build:
	cc -bundle -Wl,-undefined,dynamic_lookup -fPIC -Wall -Wextra \
	   -I$(PGINC) -I/opt/homebrew/include -Wno-ignored-attributes \
	   -o $(MODULE) $(SRCS)
	@echo "built $(MODULE) for PostgreSQL $(PG)"

install: build
	install -m 755 $(MODULE) $(PKGLIBDIR)/$(MODULE)

uninstall:
	rm -f $(PKGLIBDIR)/$(MODULE)

probe: install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v module='$(MODULE)' -f probe.sql postgres

probe2: install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v module='$(MODULE)' -f probe2.sql postgres

clean:
	rm -f $(MODULE)
