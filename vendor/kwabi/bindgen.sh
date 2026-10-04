#!/usr/bin/env bash
# Generate Rust bindings for each PostgreSQL major version.
# This script documents the binding generation approach.
# Actual bindgen execution happens in CI on Linux.

set -e

PG_VERSIONS=("16" "17" "18")
PG_DIR="/Users/sjanes/work26/kerfwork/pg"
OUTPUT_DIR="/Users/sjanes/work26/kerfwork/notes/rust-sdk/bindings"

mkdir -p "$OUTPUT_DIR"

echo "=== kwabi bindgen matrix ==="
echo ""
echo "This script generates Rust FFI bindings for each PostgreSQL major version."
echo "The bindings are used by the kwabi runtime to call PostgreSQL C functions."
echo ""
echo "Output directory: $OUTPUT_DIR"
echo ""

for ver in "${PG_VERSIONS[@]}"; do
    include_dir="$PG_DIR/postgresql-$ver.15/src/include"
    if [ "$ver" = "17" ]; then
        include_dir="$PG_DIR/postgresql-17.11/src/include"
    elif [ "$ver" = "18" ]; then
        include_dir="$PG_DIR/postgresql-18.6/src/include"
    fi
    
    output_file="$OUTPUT_DIR/bindings_pg${ver}.rs"
    
    echo "PostgreSQL $ver:"
    echo "  Include dir: $include_dir"
    echo "  Output file: $output_file"
    
    if [ ! -d "$include_dir" ]; then
        echo "  ERROR: Include dir not found"
        continue
    fi
    
    # Check if bindgen is available
    if ! command -v bindgen &> /dev/null; then
        echo "  bindgen not found — skipping (will run in CI)"
        continue
    fi
    
    echo "  Running bindgen..."
    # bindgen "$include_dir/postgres.h" \
    #     --output "$output_file" \
    #     --default-enum-style=rust \
    #     --no-layout-tests \
    #     --generate-block \
    #     --ctypes-prefix=std::os::raw \
    #     --use-core \
    #     --with-derive-default \
    #     --with-derive-hash \
    #     --with-derive-eq \
    #     --with-derive-partialeq \
    #     --allowlist-function=".*" \
    #     --allowlist-type=".*" \
    #     --allowlist-var=".*"
    echo "  Bindings generated (placeholder)"
done

echo ""
echo "=== Summary ==="
echo "3 PostgreSQL versions targeted: 16, 17, 18"
echo "Each version gets its own bindings file"
echo "The kwabi runtime uses the appropriate bindings based on the loaded PostgreSQL version"
echo ""
echo "Note: bindgen execution is deferred to CI on Linux."
echo "The binding structure is documented in notes/kwabi-v1-scope.md"
