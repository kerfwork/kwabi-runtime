# Cross-version safety test for the error channel.
#
# Proves that a v2 runtime writing into a v1 caller's smaller struct does not
# overrun it. See errsize.c for why this is the normal case rather than an edge
# case.

# pg_config per major, resolved portably. An env override wins, then a versioned
# binary on PATH, then the platform default. Mirrors shim/ci-local.sh.
PG_CONFIG_pg16 ?= $(shell command -v pg_config16 2>/dev/null || \
                      ls /usr/lib/postgresql/16/bin/pg_config 2>/dev/null || \
                      echo /opt/homebrew/opt/postgresql@16/bin/pg_config)
PG_CONFIG_pg17 ?= $(shell command -v pg_config17 2>/dev/null || \
                      ls /usr/lib/postgresql/17/bin/pg_config 2>/dev/null || \
                      echo /opt/homebrew/opt/postgresql@17/bin/pg_config)
PG_CONFIG_pg18 ?= $(shell command -v pg_config 2>/dev/null || \
                      ls /usr/lib/postgresql/18/bin/pg_config 2>/dev/null)

PG ?= 18
PG_CONFIG := $(PG_CONFIG_pg$(PG))
PGBIN     := $(shell $(PG_CONFIG) --bindir)
PGINC     := $(shell $(PG_CONFIG) --includedir-server)
PKGLIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
PSQL      := $(PGBIN)/psql

PGPORT_pg16 := 5434
PGPORT_pg17 := 5433
PGPORT_pg18 := 5432
PGPORT      := $(PGPORT_pg$(PG))

# Platform detection, matching shim/Makefile.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  DLSUFFIX    := dylib
  SHLIB_FLAGS := -bundle -Wl,-undefined,dynamic_lookup
  EXTRA_INC   := -I/opt/homebrew/include
else
  DLSUFFIX    := so
  SHLIB_FLAGS := -shared
  EXTRA_INC   :=
endif

MODULE := errsize.$(DLSUFFIX)

.PHONY: build install test clean

build:
	cc $(SHLIB_FLAGS) -fPIC -Wall -Wextra \
	   -I$(PGINC) $(EXTRA_INC) -Wno-ignored-attributes \
	   -o $(MODULE) errsize.c
	@echo "built $(MODULE)"

SUDO ?=
# Auto-detect sudo when PKGLIBDIR is not writable (e.g. GitHub runners).
ifeq ($(SUDO),)
  ifneq ($(shell test -w $(PKGLIBDIR) 2>/dev/null && echo yes),)
    SUDO := sudo
  endif
endif

install: build
	$(SUDO) install -m 755 $(MODULE) $(PKGLIBDIR)/$(MODULE)

test: install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=1 \
	  -v module='$(MODULE)' -f errsize.sql postgres

clean:
	rm -f $(MODULE)
