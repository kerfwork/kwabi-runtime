# Build the canary extension: C part + a Rust panic body.
#
# The C part includes only kwabi.h and links nothing from PostgreSQL. The Rust
# part is a separate object because Rust needs its own toolchain; the resulting
# library is still one artifact with no PostgreSQL dependency.

RUNTIME_DIR := ..
CANARY      := libcanary.dylib

.PHONY: all build clean

all: build

build:
	# Rust panic body -> object file, then everything into one shared object.
	rustc --edition 2021 --crate-type staticlib -C panic=unwind \
	      -o $(RUNTIME_DIR)/canary/libpanic_body.a panic_body.rs
	cc -shared -fPIC -Wall -Wextra -I$(RUNTIME_DIR)/.. \
	   -o $(CANARY) canary.c \
	   -Wl,-force_load,$(RUNTIME_DIR)/canary/libpanic_body.a
	@echo "built $(CANARY)"

clean:
	rm -f $(CANARY) libpanic_body.a
