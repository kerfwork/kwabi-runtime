# Build the kwabi shim for a chosen PostgreSQL major version, and run the POC.
#
#   make proof PG=18          # against the PostgreSQL 18.6 on :5432
#   make proof PG=17          # against the PostgreSQL 17.11 on :5433
#   make proof PG=16          # against the PostgreSQL 16.15 on :5434
#   make build-once           # the claim: one canary, every server
#   make ci                   # the whole local matrix
#
# The shim source is shared; only pg_config differs. That is the whole point —
# the version-specific part of the project is the build, not the code.
#
# Local clusters: 16 -> :5434, 17 -> :5433. 18 is the Homebrew service on
# :5432. See `make cluster-start`.

PG ?= 18

# Each major needs its own pg_config, port and (for the local clusters) data
# dir. Homebrew's versioned formulae are keg-only, so the linked `pg_config`
# is always the newest — every other major must be named explicitly.
PG_CONFIG_pg16 := /opt/homebrew/opt/postgresql@16/bin/pg_config
PG_CONFIG_pg17 := /opt/homebrew/opt/postgresql@17/bin/pg_config
PG_CONFIG_pg18 := pg_config
PGPORT_pg16    := 5434
PGPORT_pg17    := 5433
PGPORT_pg18    := 5432

PG_CONFIG := $(PG_CONFIG_pg$(PG))
PGPORT    := $(PGPORT_pg$(PG))

ifeq ($(PG_CONFIG),)
  $(error PG must be one of 16, 17, 18)
endif

# PG_CONFIG may be a bare command name (pg_config for 18) or a path. `wildcard`
# only resolves paths, so use `which` for the bare case.
PG_CONFIG_ABS := $(shell command -v $(PG_CONFIG) 2>/dev/null)
ifeq ($(PG_CONFIG_ABS),)
  $(error no pg_config for PG=$(PG) — try: brew install postgresql@$(PG))
endif

PGBIN     := $(shell $(PG_CONFIG) --bindir)
PGINC     := $(shell $(PG_CONFIG) --includedir-server)
PKGLIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
PSQL      := $(PGBIN)/psql

RUNTIME_DIR := ..
RELEASE     := $(RUNTIME_DIR)/target/release
# kwabi.h lives in notes/, one level above the runtime crate.
PG_CFLAGS := -I$(PGINC) -I/opt/homebrew/include -I$(RUNTIME_DIR)/.. \
             -Wno-ignored-attributes

# PostgreSQL's DLSUFFIX on macOS is .dylib even though the object is a Mach-O
# bundle, so the name must end .dylib for LOAD and AS 'lib' to find it.
BUNDLE := kwabi_runtime_pg$(PG).dylib
CANARY := libcanary.dylib

.PHONY: all build canary check install uninstall proof try panic-probe \
        guard-build guard-test guard-control cap-consumer fmgr mem-api \
        spi-api type-api subxact-probe \
        build-once ci clean info cluster-start cluster-stop cluster-status

all: build

info:
	@echo "PG=$(PG) port=$(PGPORT)"
	@echo "  pg_config  : $(PG_CONFIG)"
	@echo "  bindir     : $(PGBIN)"
	@echo "  includes   : $(PGINC)"
	@echo "  pkglibdir  : $(PKGLIBDIR)"

# -bundle + dynamic_lookup is how every real PostgreSQL module is linked on
# macOS: the object carries undefined symbols, resolved by the postgres
# process at load time. -bundle_loader would work but would bind the runtime
# to one specific postgres binary, defeating the build-once property.
build: $(RELEASE)/libkwabi_runtime.a
	cc -bundle -Wl,-undefined,dynamic_lookup -fPIC -Wall -Wextra \
	   $(PG_CFLAGS) -I$(RUNTIME_DIR) \
	   -o $(BUNDLE) kwabi_runtime_shim.c \
	   $(RELEASE)/libkwabi_runtime.a
	@echo "built $(BUNDLE) for PostgreSQL $(PG)"

# Depend on the crate's sources, not just on the target existing.
#
# A rule with no prerequisites is "satisfied" the moment the file exists, so
# `make` never rebuilt this archive after `src/lib.rs` changed. The shim was
# then relinked against a stale runtime: new slots stayed NULL in the published
# table and the failure looked like a wiring bug rather than a build one.
RUNTIME_SRC := $(wildcard $(RUNTIME_DIR)/src/*.rs) $(RUNTIME_DIR)/Cargo.toml

$(RELEASE)/libkwabi_runtime.a: $(RUNTIME_SRC)
	cargo build --release --manifest-path $(RUNTIME_DIR)/Cargo.toml

# The extension: includes only kwabi.h, links no PostgreSQL symbol. Built
# ONCE — this target does not depend on PG, which is the claim being tested.
canary:
	cc -shared -fPIC -Wall -Wextra -I$(RUNTIME_DIR)/.. \
	   -o $(RUNTIME_DIR)/canary/$(CANARY) $(RUNTIME_DIR)/canary/canary.c
	@echo "built canary/$(CANARY) (version-independent)"

# Assert the canary really has no PostgreSQL dependency.
check: canary
	@test -z "$$(nm -u $(RUNTIME_DIR)/canary/$(CANARY))" \
	  && echo "OK: canary has zero undefined symbols" \
	  || (echo "FAIL: canary has undefined symbols:"; \
	      nm -u $(RUNTIME_DIR)/canary/$(CANARY); exit 1)

install: build canary
	install -m 755 $(BUNDLE) $(PKGLIBDIR)/$(BUNDLE)
	install -m 755 $(RUNTIME_DIR)/canary/$(CANARY) $(PKGLIBDIR)/$(CANARY)
	@echo "installed both into $(PKGLIBDIR)"

uninstall:
	rm -f $(PKGLIBDIR)/$(BUNDLE) $(PKGLIBDIR)/$(CANARY)

proof: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f proof.sql postgres

# kwabi_try proof. Separate from `proof` because the panic case in it is a
# known limitation that aborts the backend; see panic-probe.sql.
try: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f try.sql postgres

# Guarded-body tests: the panic containment that #[guarded_body] provides.
GUARDED := libguarded_canary.dylib

guard-build:
	cd $(RUNTIME_DIR)/canary/guarded && cargo build --release
	install -m 755 $(RUNTIME_DIR)/canary/guarded/target/release/$(GUARDED) $(PKGLIBDIR)/$(GUARDED)

guard-test: check install guard-build
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v libdir='$(PKGLIBDIR)' -v guarded='$(GUARDED)' \
	  -f guard-test.sql postgres

# The capability bitset, consumed by an extension rather than read by the shim.
# Needs the same guarded canary, because the consumer is one of its bodies.
cap-consumer: check install guard-build
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v libdir='$(PKGLIBDIR)' -v guarded='$(GUARDED)' \
	  -f capability-consumer.sql postgres

# The function-call group: lookup, 1/2/3/N-arg calls, NULL results, caught errors.
fmgr: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f fmgr-api.sql postgres

# The memory context lifecycle: create -> switch -> alloc -> name -> reset -> delete.
mem-api: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f mem-api.sql postgres

# The SPI group: execute queries, read results, handle errors.
spi-api: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f spi-api.sql postgres

type-api: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f type-api.sql postgres

# The two-way control for fmgr-api check 13: builds a bundle with the
# subtransaction removed and requires the partial write to SURVIVE there.
# Without this, check 13 is a check that has only ever passed.
subxact-probe: check install
	./subxact-probe.sh $(PG)

# The control: must abort. Run separately.
guard-control: check install guard-build
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v libdir='$(PKGLIBDIR)' -v guarded='$(GUARDED)' \
	  -f guard-control.sql postgres || true
	@echo "(the abort above is the documented control, not a failure)"

# The panic limitation, isolated. Expect the connection to drop.
panic-probe: check install
	$(PSQL) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f panic-probe.sql postgres || true
	@echo "(the connection loss above is the documented finding, not a failure)" 

# The build-once gate: build the canary once, then load that same file into
# every server. If it passes everywhere, the claim is demonstrated rather than
# asserted.
build-once: check
	@echo "=== canary hash (must not change between runs) ==="
	@shasum -a 256 $(RUNTIME_DIR)/canary/$(CANARY)
	@for v in 16 17 18; do \
	  $(MAKE) --no-print-directory proof PG=$$v || exit 1; \
	done

# The local matrix: core tests, canary pinning, per-major build, and the
# checks in each live server.
ci:
	./ci-local.sh

clean:
	rm -f $(BUNDLE)

# ---------------------------------------------------------------------------
# The local clusters, for anyone starting from scratch.
#
#   brew install postgresql@16 postgresql@17   # keg-only: do not shadow @18
#   make cluster-start
#
# macOS note: PostgreSQL 17 and 18 refuse to start without a valid locale
# ("postmaster became multithreaded during startup"). LC_ALL must be set in
# the server's environment, not just the client's.
# ---------------------------------------------------------------------------

CLUSTERS := 16 17

cluster-start:
	@for v in $(CLUSTERS); do \
	  bin=/opt/homebrew/opt/postgresql@$$v/bin; \
	  data=$(CURDIR)/../pg$$v/data; \
	  port=$$(if [ $$v = 16 ]; then echo 5434; else echo 5433; fi); \
	  test -x $$bin/initdb || { echo "run: brew install postgresql@$$v"; exit 1; }; \
	  if [ ! -f $$data/PG_VERSION ]; then \
	    mkdir -p $$(dirname $$data); \
	    $$bin/initdb -D $$data -U $$(whoami) --encoding=UTF8 --locale=C >/dev/null; \
	    printf '\nport = %s\nlisten_addresses = %s\nunix_socket_directories = %s\n' \
	      $$port "'localhost'" "'/tmp'" >> $$data/postgresql.conf; \
	  fi; \
	  LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
	    $$bin/pg_ctl -D $$data -l $$data/server.log start >/dev/null 2>&1; \
	  sleep 1; \
	  $$bin/pg_isready -p $$port; \
	done

cluster-stop:
	@for v in $(CLUSTERS); do \
	  bin=/opt/homebrew/opt/postgresql@$$v/bin; \
	  $$bin/pg_ctl -D $(CURDIR)/../pg$$v/data stop >/dev/null 2>&1 || true; \
	done

cluster-status:
	@for v in 16 17 18; do \
	  bin=$$(if [ $$v = 18 ]; then echo $(shell pg_config --bindir); \
	        else echo /opt/homebrew/opt/postgresql@$$v/bin; fi); \
	  port=$$(if [ $$v = 16 ]; then echo 5434; \
	          elif [ $$v = 17 ]; then echo 5433; else echo 5432; fi); \
	  printf 'PG %s :%s  ' $$v $$port; \
	  $$bin/pg_isready -p $$port >/dev/null 2>&1 && echo up || echo down; \
	done
