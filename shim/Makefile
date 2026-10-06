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

# Each major needs its own pg_config. Resolution is portable and ordered:
# an explicit env/Make override wins, then a versioned binary on PATH, then the
# platform default (Homebrew's keg-only formulae on macOS, /usr/lib on Debian).
# Using ?= means the caller can always override, which is what ci-local.sh does.
PG_CONFIG_pg16 ?= $(shell command -v pg_config16 2>/dev/null || \
                     ls /usr/lib/postgresql/16/bin/pg_config 2>/dev/null || \
                     echo /opt/homebrew/opt/postgresql@16/bin/pg_config)
PG_CONFIG_pg17 ?= $(shell command -v pg_config17 2>/dev/null || \
                     ls /usr/lib/postgresql/17/bin/pg_config 2>/dev/null || \
                     echo /opt/homebrew/opt/postgresql@17/bin/pg_config)
PG_CONFIG_pg18 ?= $(shell command -v pg_config 2>/dev/null || \
                     ls /usr/lib/postgresql/18/bin/pg_config 2>/dev/null)
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
PG_CONFIG_ABS := $(shell command -v $(PG_CONFIG) 2>/dev/null || ls $(PG_CONFIG) 2>/dev/null)
ifeq ($(PG_CONFIG_ABS),)
  $(error no pg_config for PG=$(PG) — install postgresql-server-dev-$(PG) or set PG_CONFIG_pg$(PG))
endif

PGBIN     := $(shell $(PG_CONFIG) --bindir)
PGINC     := $(shell $(PG_CONFIG) --includedir-server)
PKGLIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
PSQL      := $(PGBIN)/psql

RUNTIME_DIR := ..
RELEASE     := $(RUNTIME_DIR)/target/release

# The unix-socket directory differs by platform (macOS /tmp, Debian
# /var/run/postgresql). Probe for a live socket, then fall back to the
# platform default; PGHOST overrides.
PSOCK ?= $(shell for d in /var/run/postgresql /tmp; do \
                    ls $$d/.s.PGSQL.* >/dev/null 2>&1 && { echo $$d; break; }; \
                  done | head -1)
ifeq ($(PSOCK),)
  PSOCK := $(shell [ -d /var/run/postgresql ] && echo /var/run/postgresql || echo /tmp)
endif

# Platform detection. macOS and Linux differ in three ways that matter here:
# the shared-object suffix, the linker flags for an undefined-symbol module,
# and where PostgreSQL's headers live. Everything platform-specific is derived
# here so no other rule needs to care.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  DLSUFFIX  := dylib
  # -bundle + dynamic_lookup is how every real PostgreSQL module is linked on
  # macOS: the object carries undefined symbols, resolved by the postgres
  # process at load time. -bundle_loader would work but would bind the runtime
  # to one specific postgres binary, defeating the build-once property.
  SHLIB_FLAGS := -bundle -Wl,-undefined,dynamic_lookup
  EXTRA_INC   := -I/opt/homebrew/include
  # macOS ld resolves the Rust allocator shims from the archive on demand.
  RUST_LINK_A := $(RELEASE)/libkwabi_runtime.a
else
  DLSUFFIX  := so
  # On Linux the same effect is -shared with undefined symbols left undefined,
  # which is the default for a shared object.
  SHLIB_FLAGS := -shared
  EXTRA_INC   :=
  # Linux ld does not pull the Rust allocator shims (__rdl_alloc and friends)
  # out of the archive on demand, so the link fails with "undefined reference
  # to __rustc::__rdl_alloc". --whole-archive forces every object in, which is
  # what the macOS path gets implicitly. --no-whole-archive closes the scope so
  # libc is unaffected.
  RUST_LINK_A := -Wl,--whole-archive $(RELEASE)/libkwabi_runtime.a -Wl,--no-whole-archive
endif

# Where the vendored kwabi header lives. The private source tree keeps it under
# vendor/kwabi/include/, while the published repo vendors the SDK crate with the
# header at vendor/kwabi/kwabi.h. Resolve whichever is present so the same
# Makefile works in both, rather than forcing one layout on the other.
KWABI_INC := $(firstword $(wildcard $(RUNTIME_DIR)/vendor/kwabi/include) $(RUNTIME_DIR)/vendor/kwabi)

# kwabi.h lives in notes/, one level above the runtime crate.
PG_CFLAGS := -I$(PGINC) $(EXTRA_INC) -I$(KWABI_INC) \
             -Wno-ignored-attributes

BUNDLE := kwabi_runtime_pg$(PG).$(DLSUFFIX)
CANARY := libcanary.$(DLSUFFIX)

.PHONY: all build canary check install uninstall proof try panic-probe \
        guard-build guard-test guard-control capabilities cap-consumer fmgr mem-api \
        spi-api type-api guc-api defrem-api parser-api subxact-probe \
        relation-api node-tree-api errsize \
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
	cc $(SHLIB_FLAGS) -fPIC -Wall -Wextra \
	   $(PG_CFLAGS) -I$(RUNTIME_DIR) \
	   -o $(BUNDLE) kwabi_runtime_shim.c \
	   $(RUST_LINK_A)
	@echo "built $(BUNDLE) for PostgreSQL $(PG)"

# Depend on the crate's sources, not just on the target existing.
#
# A rule with no prerequisites is "satisfied" the moment the file exists, so
# `make` never rebuilt this archive after `src/lib.rs` changed. The shim was
# then relinked against a stale runtime: new slots stayed NULL in the published
# table and the failure looked like a wiring bug rather than a build one.
RUNTIME_SRC := $(wildcard $(RUNTIME_DIR)/src/*.rs) $(RUNTIME_DIR)/Cargo.toml

# A cargo-built archive is platform-specific, but it lives in a directory that
# may be shared between macOS and Linux (a Docker bind-mount, a network home
# dir). If the archive was produced on the other platform, make sees it as
# up-to-date and the link fails with "archive has no index" or "member ... is
# not an object".
#
# Timestamps are not reliable (a foreign archive can be newer than anything),
# and sniffing archive bytes is not portable (macOS `ar` and GNU `ar` lay
# members out differently). So the build records which platform produced the
# archive, in a marker next to it, and a mismatch removes the archive at parse
# time -- before make decides what is up to date.
PLATFORM_MARKER := $(RELEASE)/.kwabi-platform

PLATFORM_OK := $(shell [ -f $(PLATFORM_MARKER) ] && \
                      [ "$$(cat $(PLATFORM_MARKER) 2>/dev/null)" = "$(UNAME_S)" ] && echo yes)

ifneq ($(PLATFORM_OK),yes)
  $(shell rm -f $(RELEASE)/libkwabi_runtime.a $(PLATFORM_MARKER))
endif

$(RELEASE)/libkwabi_runtime.a: $(RUNTIME_SRC)
	cargo build --release --manifest-path $(RUNTIME_DIR)/Cargo.toml
	@ranlib $@ 2>/dev/null || true
	@mkdir -p $(RELEASE); echo "$(UNAME_S)" > $(PLATFORM_MARKER)

# The extension: includes only kwabi.h, links no PostgreSQL symbol. Built
# ONCE — this target does not depend on PG, which is the claim being tested.
canary:
	cc -shared -fPIC -Wall -Wextra -I$(KWABI_INC) \
	   -o $(RUNTIME_DIR)/canary/$(CANARY) $(RUNTIME_DIR)/canary/canary.c
	@echo "built canary/$(CANARY) (version-independent)"

# Assert the canary really has no PostgreSQL dependency.
#
# The claim is "links no PostgreSQL symbol", so that is what is asserted — not
# "zero undefined symbols". A macOS -shared object genuinely has none, which is
# why the stricter form used to pass; a Linux shared object always carries a few
# weak libc/init symbols (__cxa_finalize, __gmon_start__, the ITM clone table),
# and requiring none would fail there for no real reason.
check: canary
	@BAD=$$(nm -u $(RUNTIME_DIR)/canary/$(CANARY) 2>/dev/null \
	         | grep -iE 'palloc|pfree|elog|ereport|SPI_|heap_|_PG_|Relation|TupleDesc|MemoryContext' || true); \
	 if [ -z "$$BAD" ]; then \
	   echo "OK: canary references no PostgreSQL symbol"; \
	 else \
	   echo "FAIL: canary references PostgreSQL symbols:"; echo "$$BAD"; exit 1; \
	 fi

SUDO ?=
# Auto-detect sudo when PKGLIBDIR is not writable (e.g. GitHub runners).
ifeq ($(SUDO),)
  ifeq ($(shell test -w $(PKGLIBDIR) 2>/dev/null && echo yes),)
    SUDO := sudo
  endif
endif

install: build canary
	$(SUDO) install -m 755 $(BUNDLE) $(PKGLIBDIR)/$(BUNDLE)
	$(SUDO) install -m 755 $(RUNTIME_DIR)/canary/$(CANARY) $(PKGLIBDIR)/$(CANARY)
	@echo "installed both into $(PKGLIBDIR)"

uninstall:
	rm -f $(PKGLIBDIR)/$(BUNDLE) $(PKGLIBDIR)/$(CANARY)

proof: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f proof.sql postgres

# kwabi_try proof. Separate from `proof` because the panic case in it is a
# known limitation that aborts the backend; see panic-probe.sql.
try: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f try.sql postgres

# Guarded-body tests: the panic containment that #[guarded_body] provides.
GUARDED := libguarded_canary.$(DLSUFFIX)

guard-build:
	cd $(RUNTIME_DIR)/canary/guarded && cargo build --release
	$(SUDO) install -m 755 $(RUNTIME_DIR)/canary/guarded/target/release/$(GUARDED) $(PKGLIBDIR)/$(GUARDED)
.PHONY: guard-build

guard-test: check install guard-build
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v libdir='$(PKGLIBDIR)' -v guarded='$(GUARDED)' \
	  -f guard-test.sql postgres

# The capability bitset, consumed by an extension rather than read by the shim.
# Needs the same guarded canary, because the consumer is one of its bodies.
cap-consumer: check install guard-build
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v libdir='$(PKGLIBDIR)' -v guarded='$(GUARDED)' \
	  -f capability-consumer.sql postgres

# The capability bitset, read by the shim.
capabilities: check install guard-build
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=1 \
	  -v bundle='$(BUNDLE)' \
	  -v canary='$(PKGLIBDIR)/libguarded_canary.$(DLSUFFIX)' \
	  -f capabilities.sql postgres

# The GUC group: read and write configuration values through the ABI.
guc-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f guc-api.sql postgres

# The defrem group: create, alter, and drop column defaults through the ABI.
defrem-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f defrem-api.sql postgres

# The parser group: parse expressions, parse type names, query operator properties.
parser-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f parser-api.sql postgres

# The errsize group: cross-version error channel safety.
errsize:
	$(MAKE) --no-print-directory -C ../errsize test PG=$(PG)

# The function-call group: lookup, 1/2/3/N-arg calls, NULL results, caught errors.
fmgr: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f fmgr-api.sql postgres

# The memory context lifecycle: create -> switch -> alloc -> name -> reset -> delete.
mem-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f mem-api.sql postgres

# The SPI group: execute queries, read results, handle errors.
spi-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f spi-api.sql postgres

type-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v canary='$(CANARY)' -v libdir='$(PKGLIBDIR)' \
	  -f type-api.sql postgres

# The tuple/slot group: tuple descriptor, heap tuple, and slot accessors.
tuple-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f tuple-api.sql postgres

# The relation cache group: open, read metadata, close.
relation-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f relation-api.sql postgres

# The node tree group: parse, inspect node types, query fields.
node-tree-api: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' \
	  -f node-tree-api.sql postgres

# The two-way control for fmgr-api check 13: builds a bundle with the
# subtransaction removed and requires the partial write to SURVIVE there.
# Without this, check 13 is a check that has only ever passed.
subxact-probe: check install
	./subxact-probe.sh $(PG)

# The control: must abort. Run separately.
guard-control: check install guard-build
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
	  -v bundle='$(BUNDLE)' -v libdir='$(PKGLIBDIR)' -v guarded='$(GUARDED)' \
	  -f guard-control.sql postgres || true
	@echo "(the abort above is the documented control, not a failure)"

# The panic limitation, isolated. Expect the connection to drop.
panic-probe: check install
	$(PSQL) -h $(PSOCK) -p $(PGPORT) -v ON_ERROR_STOP=0 \
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

# Bin directory for a major, resolved the same way as PG_CONFIG: env override,
# then a versioned binary on PATH, then the platform default. The cluster
# helpers use this so they work on Linux as well as macOS.
BIN_pg16 ?= $(shell dirname $$(command -v pg_config16 2>/dev/null || echo /usr/lib/postgresql/16/bin/pg_config) 2>/dev/null)
BIN_pg17 ?= $(shell dirname $$(command -v pg_config17 2>/dev/null || echo /usr/lib/postgresql/17/bin/pg_config) 2>/dev/null)
BIN_pg18 ?= $(shell dirname $$(command -v pg_config 2>/dev/null || echo /usr/lib/postgresql/18/bin/pg_config) 2>/dev/null)

cluster-start:
	@for v in $(CLUSTERS); do \
	  bin=$(BIN_pg$$v); \
	  data=$(CURDIR)/../pg$$v/data; \
	  port=$$(if [ $$v = 16 ]; then echo 5434; else echo 5433; fi); \
	  test -x $$bin/initdb || { echo "no initdb for PG$$v (install postgresql-server-dev-$$v)"; exit 1; }; \
	  if [ ! -f $$data/PG_VERSION ]; then \
	    mkdir -p $$(dirname $$data); \
	    $$bin/initdb -D $$data -U $$(whoami) --encoding=UTF8 --locale=C >/dev/null; \
	    printf '\nport = %s\nlisten_addresses = %s\nunix_socket_directories = %s\n' \
	      $$port "'localhost'" "'$(PSOCK)'" >> $$data/postgresql.conf; \
	  fi; \
	  LC_ALL="en_US.UTF-8" LANG="en_US.UTF-8" \
	    $$bin/pg_ctl -D $$data -l $$data/server.log start >/dev/null 2>&1; \
	  sleep 1; \
	  $$bin/pg_isready -h $(PSOCK) -p $$port; \
	done

cluster-stop:
	@for v in $(CLUSTERS); do \
	  bin=$(BIN_pg$$v); \
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
