#!/usr/bin/env bash
# One-shot Gadgetron ARKS recon driver.
# Run inside the gadgetron_dev container, after starting `gadgetron -p<port>`
# in a separate terminal.
set -euo pipefail

INPUT=""
CONFIG=""
SERVER_PORT=9030
EXPORT_USER_INT=0

SERVER_HOST="localhost"
NOISE_CFG="default_measurement_dependencies.xml"
XML_SRC_DIR="/work/gadgetron_source"
XML_DST_DIR="/opt/conda/envs/gadgetron/share/gadgetron/config"
OUT_ROOT="/work"
H5_TO_NII="/work/gadgetron_source/h5_to_nii.py"
EXPORT_USER_INT_PY="/work/gadgetron_source/export_user_int.py"

usage() {
  cat <<EOF
Usage: $0 -i INPUT -c CONFIG_XML [-p SERVER_PORT] [-u]

  -i INPUT       .dat   -> runs siemens_to_ismrmrd, then recon
                 _2.h5  -> skips preprocessing, goes straight to recon
                          (a sibling _1.h5 must exist for noise calibration)
                 Filename must contain FIDxxxxx (used to name the output dir).
  -c CONFIG_XML  recon XML config. Either:
                   - basename my_config.xml (looked up in $XML_SRC_DIR)
                   - full path /any/where/my_config.xml
                 Always cp'd to $XML_DST_DIR every run.
  -p SERVER_PORT gadgetron server port (default: $SERVER_PORT)
  -u             also run export_user_int.py on the preprocessed _2.h5
  -h             show this help

Output: $OUT_ROOT/fcrl_out_<FID>/ — all artifacts go here
        (preprocessed _1.h5 / _2.h5, recon .h5, .nii, optional user_int .csv)

Server: assumes 'gadgetron -p<port>' is already running in another terminal.
EOF
}

while getopts "i:c:p:uh" opt; do
  case $opt in
    i) INPUT=$OPTARG ;;
    c) CONFIG=$OPTARG ;;
    p) SERVER_PORT=$OPTARG ;;
    u) EXPORT_USER_INT=1 ;;
    h) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

if [[ -z $INPUT || -z $CONFIG ]]; then
  usage; exit 1
fi

# --- derive FID tag and output dir ---
FID_TAG=$(echo "$(basename "$INPUT")" | grep -oE 'FID[0-9]+' | head -1 || true)
if [[ -z $FID_TAG ]]; then
  echo "ERROR: could not extract FID<number> from input filename: $(basename "$INPUT")" >&2
  exit 1
fi
OUT_DIR="$OUT_ROOT/fcrl_out_$FID_TAG"
mkdir -p "$OUT_DIR"

# --- resolve XML src/dst (basename or full path) ---
if [[ "$CONFIG" == */* ]]; then
  XML_SRC="$CONFIG"
else
  XML_SRC="$XML_SRC_DIR/$CONFIG"
fi
XML_BASENAME="$(basename "$XML_SRC")"
XML_DST="$XML_DST_DIR/$XML_BASENAME"

# --- sanity: server reachable? ---
if ! (echo > /dev/tcp/$SERVER_HOST/$SERVER_PORT) 2>/dev/null; then
  echo "ERROR: gadgetron server not reachable on $SERVER_HOST:$SERVER_PORT" >&2
  echo "Open another terminal and run:  gadgetron -p$SERVER_PORT" >&2
  exit 1
fi

# --- sanity: XML and helper scripts exist ---
[[ -f $XML_SRC ]]            || { echo "ERROR: config not found: $XML_SRC" >&2; exit 1; }
[[ -f $H5_TO_NII ]]          || { echo "ERROR: helper not found: $H5_TO_NII" >&2; exit 1; }
if (( EXPORT_USER_INT )); then
  [[ -f $EXPORT_USER_INT_PY ]] || { echo "ERROR: helper not found: $EXPORT_USER_INT_PY" >&2; exit 1; }
fi

echo "[setup] FID=$FID_TAG  out=$OUT_DIR  port=$SERVER_PORT"

# --- Stage 4: preprocess (only if input is .dat) ---
case "${INPUT##*.}" in
  dat)
    [[ -f $INPUT ]] || { echo "ERROR: .dat not found: $INPUT" >&2; exit 1; }
    PREP_BASE="$OUT_DIR/meas_${FID_TAG}_preprocessed"
    H5_NOISE="${PREP_BASE}_1.h5"
    H5_DATA="${PREP_BASE}_2.h5"
    echo "[stage 4] siemens_to_ismrmrd -> ${PREP_BASE}_{1,2}.h5"
    siemens_to_ismrmrd -f "$INPUT" -o "${PREP_BASE}.h5" -Z
    ;;
  h5)
    [[ "$INPUT" == *_2.h5 ]] || { echo "ERROR: .h5 input must end in _2.h5 (got: $INPUT)" >&2; exit 1; }
    H5_DATA="$INPUT"
    H5_NOISE="${INPUT%_2.h5}_1.h5"
    [[ -f $H5_NOISE ]] || { echo "ERROR: noise file not found: $H5_NOISE" >&2; exit 1; }
    echo "[stage 4] skipped (input is .h5)"
    ;;
  *)
    echo "ERROR: input must be .dat or .h5 (got: $INPUT)" >&2; exit 1
    ;;
esac

# --- Stage 5: deploy XML, run noise calibration, run recon ---
echo "[stage 5] cp $XML_SRC -> $XML_DST"
cp "$XML_SRC" "$XML_DST"

echo "[stage 5a] noise calibration: $H5_NOISE"
gadgetron_ismrmrd_client \
  -a "$SERVER_HOST" -p "$SERVER_PORT" \
  -f "$H5_NOISE" -c "$NOISE_CFG"

RECON_H5="$OUT_DIR/fcrl_out_${FID_TAG}.h5"
echo "[stage 5b] recon: $H5_DATA -> $RECON_H5"
gadgetron_ismrmrd_client \
  -a "$SERVER_HOST" -p "$SERVER_PORT" \
  -f "$H5_DATA" -c "$XML_BASENAME" \
  -o "$RECON_H5"

# --- Stage 6: h5 -> nii ---
echo "[stage 6] h5_to_nii: $RECON_H5"
python3 "$H5_TO_NII" "$RECON_H5"

# --- Stage 7 (optional): dump user_int from preprocessed _2.h5 ---
if (( EXPORT_USER_INT )); then
  USER_INT_CSV="$OUT_DIR/fcrl_out_${FID_TAG}_user_int.csv"
  echo "[stage 7] export_user_int: $H5_DATA -> $USER_INT_CSV"
  python3 "$EXPORT_USER_INT_PY" "$H5_DATA" "$USER_INT_CSV"
fi

echo "[done] all artifacts in $OUT_DIR"
ls -lh "$OUT_DIR"
