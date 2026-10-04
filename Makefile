# kwabi runtime-skeleton — build and proof
#
# `make proof` is the whole demonstration: build the runtime, build a C
# extension that includes only kwabi.h, and drive it through the ABI.

RUNTIME_DIR := $(shell cd .. && pwd)
RELEASE := target/release

.PHONY: all test build canary host proof check-symbols clean

all: proof

## Rust-side tests: layout, publication, null slots, error firewall
test:
	cargo test

build:
	cargo build --release

## The extension: includes only kwabi.h, links no PostgreSQL symbol
canary: $(RELEASE)/libkwabi_runtime.dylib
	cc -shared -fPIC -Wall -Wextra -I$(RUNTIME_DIR) \
	   -o canary/libcanary.dylib canary/canary.c

## The host: stands in for PostgreSQL
host: $(RELEASE)/libkwabi_runtime.dylib
	cc -Wall -Wextra -I$(RUNTIME_DIR) -o canary/host canary/host.c \
	   -L$(RELEASE) -lkwabi_runtime -Wl,-rpath,$(CURDIR)/$(RELEASE)

$(RELEASE)/libkwabi_runtime.dylib:
	cargo build --release

## Assert the canary has no external dependencies. This is the build-once
## claim made checkable: an object with zero undefined symbols cannot be
## secretly linked against PostgreSQL.
check-symbols: canary
	@echo "undefined symbols in libcanary.dylib:"
	@nm -u canary/libcanary.dylib || true
	@test -z "$$(nm -u canary/libcanary.dylib)" \
	  && echo "OK: zero external dependencies" \
	  || (echo "FAIL: canary has undefined symbols"; exit 1)

proof: build canary host check-symbols
	@echo
	./canary/host ./canary/libcanary.dylib

clean:
	cargo clean
	rm -f canary/libcanary.dylib canary/host
