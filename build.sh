#!/usr/bin/env bash
# One-shot build & install for the FCRL Gadgetron source tree.
# Run inside the gadgetron_dev container with the gadgetron conda env activated.
set -euo pipefail

SRC_DIR="/work/gadgetron_source"
BUILD_DIR="$SRC_DIR/build"

if [[ -z "${CONDA_PREFIX:-}" ]]; then
  echo "ERROR: CONDA_PREFIX is not set. Activate the env first:" >&2
  echo "  source /opt/conda/etc/profile.d/conda.sh && conda activate gadgetron" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "[cmake] configure -> $BUILD_DIR  (install prefix: $CONDA_PREFIX)"
cmake -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_MKL=ON \
  -DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX" \
  ..

echo "[ninja] build"
ninja

echo "[ninja] install"
ninja install

echo "[done] installed to $CONDA_PREFIX"
