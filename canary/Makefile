# Build the canary extension: C part + a Rust panic body.
#
# The C part includes only kwabi.h and links nothing from PostgreSQL. The Rust
# part is a separate object because Rust needs its own toolchain; the resulting
# library is still one artifact with no PostgreSQL dependency.

RUNTIME_DIR := ..

# Platform detection, matching shim/Makefile. The two must agree or the canary
# is built under one name and looked for under another.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  DLSUFFIX := dylib
  # macOS: force_load the static archive so the panic body is pulled in whole.
  FORCE_LOAD := -Wl,-force_load,$(RUNTIME_DIR)/canary/libpanic_body.a
else
  DLSUFFIX := so
  # Linux ld has no -force_load; --whole-archive does the same job. The archive
  # must be wrapped by --no-whole-archive so libc and friends are not affected.
  FORCE_LOAD := -Wl,--whole-archive $(RUNTIME_DIR)/canary/libpanic_body.a -Wl,--no-whole-archive
endif

CANARY := libcanary.$(DLSUFFIX)

.PHONY: all build clean

all: build

build:
	# Rust panic body -> object file, then everything into one shared object.
	rustc --edition 2021 --crate-type staticlib -C panic=unwind \
	      -o $(RUNTIME_DIR)/canary/libpanic_body.a panic_body.rs
	cc -shared -fPIC -Wall -Wextra -I$(RUNTIME_DIR)/vendor/kwabi \
	   -o $(CANARY) canary.c \
	   $(FORCE_LOAD)
	@echo "built $(CANARY)"

clean:
	rm -f $(CANARY) libpanic_body.a
