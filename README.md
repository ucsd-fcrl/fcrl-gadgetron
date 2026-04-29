# Gadgetron Image Reconstruction Framework

The Gadgetron is an open source project for medical image reconstruction. If you find the Gadgetron useful in your research, please cite:

Hansen MS, Sørensen TS. Gadgetron: An Open Source Framework for Medical Image Reconstruction. Magn Reson Med. 2013 Jun;69(6):1768-76.

Docs: [https://gadgetron.readthedocs.io](https://gadgetron.readthedocs.io). License: modified MIT (see `LICENSE`).

---

# FCRL Golden Radial Mode4 Reconstruction Pipeline

Custom Gadgetron pipeline for FCRL golden radial acquisitions. Two reconstruction modes:

| XML config | Behavior |
|---|---|
| `fcrl_golden_radial_mode4_read_angle_realtime.xml` | Reads trajectory angles from acquisition headers |
| `fcrl_golden_radial_mode4_arks_realtime.xml` | ARKS (Adaptive Radial K-Space) — gathers historical k-space spokes from a deque buffer and merges them with the current frame for improved under-sampled recon |

ARKS implementation lives in `gadgets/radial/gpuRadialPrepGadget.{cpp,h}` (class `gpuRadialSensePrepGadget`).

---

## Recent updates

- **`recon.sh`**: one-shot wrapper for the entire recon pipeline (see below).
- **ARKS recon**: spoke-buffer reconstruction added to `gpuRadialPrepGadget`. Uses scanner `user_int[3]` (N_samples) and `user_int[4..7]` (lag offsets to historical spoke groups). Logs to `/work/arks_log.txt`.
- **Helpers**:
  - `h5_to_nii.py` — convert recon `.h5` to `.nii` (auto-installs deps)
  - `export_user_int.py` — dump raw `user_int[8]` / `user_float[8]` from a preprocessed `.h5` to CSV

---

## Build / install (after editing gadget source)

```bash
cd /work/gadgetron_source/build
cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DUSE_MKL=ON \
      -DCMAKE_INSTALL_PREFIX=${CONDA_PREFIX} ..
ninja && ninja install
```

---

## One-shot recon: `recon.sh`

Wrapper that handles preprocessing → noise calibration → recon → `h5→nii` in one call. All artifacts land in `/work/fcrl_out_<FID>/`.

**Step 1 — start the server in a separate terminal (kept alive):**
```bash
docker exec -it gadgetron_dev bash
source /opt/conda/etc/profile.d/conda.sh && conda activate gadgetron
gadgetron -p9030
```

**Step 2 — run recon:**
```bash
# From .dat (full pipeline)
/work/recon.sh \
  -i /projects/arks_recon/dat_files/meas_MID00129_FID01458_...dat \
  -c fcrl_golden_radial_mode4_arks_realtime.xml

# From _2.h5 (skip siemens_to_ismrmrd; sibling _1.h5 must exist)
/work/recon.sh \
  -i /work/fcrl_out_FID01458/meas_FID01458_preprocessed_2.h5 \
  -c fcrl_golden_radial_mode4_arks_realtime.xml

# With non-default port and user_int CSV export
/work/recon.sh -i ...dat -c <xml> -p 9088 -u
```

**Flags:**
- `-i` input — `.dat` (runs full pipeline) or `_2.h5` (skips preprocessing). Filename must contain `FID<num>`.
- `-c` XML config — basename (looked up in `/work/gadgetron_source/`) or full path. **Auto-copied** to `/opt/conda/envs/gadgetron/share/gadgetron/config/` every run, so live edits always take effect.
- `-p` server port (default `9030`)
- `-u` also run `export_user_int.py` on the preprocessed data h5

**Output (example, FID01458):**
```
/work/fcrl_out_FID01458/
├── meas_FID01458_preprocessed_1.h5     # noise (only when starting from .dat)
├── meas_FID01458_preprocessed_2.h5     # data  (only when starting from .dat)
├── fcrl_out_FID01458.h5                # recon result
├── fcrl_out_FID01458.nii               # auto-converted from .h5
└── fcrl_out_FID01458_user_int.csv      # only with -u
```

Re-running into the same folder overwrites same-named files but keeps others. To start clean, `rm -rf /work/fcrl_out_FID01458` first.

---

## Manual workflow (reference, if not using `recon.sh`)

```bash
CFG=fcrl_golden_radial_mode4_arks_realtime.xml
CFG_noise=default_measurement_dependencies.xml

# 1. Convert .dat -> ISMRMRD .h5  (-Z splits into _1.h5 noise + _2.h5 data)
siemens_to_ismrmrd -f input.dat -o output.h5 -Z

# 2. Deploy XML
cp /work/gadgetron_source/$CFG /opt/conda/envs/gadgetron/share/gadgetron/config/

# 3. Noise calibration, then recon
gadgetron_ismrmrd_client -a localhost -p 9030 -f output_1.h5 -c "$CFG_noise"
gadgetron_ismrmrd_client -a localhost -p 9030 -f output_2.h5 -c "$CFG" -o recon_out.h5
```
