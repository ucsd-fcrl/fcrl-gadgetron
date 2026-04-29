
---

## Commit `8daec552` — added xml file and readme for recon
**Date:** Mar 6, 2026 (33 minutes after the previous commit)

No code changes — this commit adds two new files: a **Gadgetron pipeline XML config** and a **README** documenting the full workflow. This is the first time the complete reconstruction pipeline is formally defined.

---

### 1. `fcrl_golden_radial_mode4_read_angle_realtime.xml` — the pipeline config

This defines the full gadget chain for FCRL ARKS reconstruction:

```
GadgetIsmrmrdAcquisitionMessageReader   (slot 1008)
GadgetIsmrmrdWaveformMessageReader      (slot 1026)
        │
        ▼
NoiseAdjustGadget
        │
        ▼
PCACoilGadget
        │
        ▼
CoilReductionGadget  →  coils_out = 8
        │
        ▼
gpuRadialSensePrepGadget  (mode 4, device 1)
        │
        ▼
gpuLALMSenseGadget  (slice 0, device 1)
gpuLALMSenseGadget  (slice 1, device 1)
gpuLALMSenseGadget  (slice 2, device 1)
        │
        ▼
ExtractGadget
        │
        ▼
ImageFinishGadget
MRIImageWriter  (slot 1022)
```

#### Key parameters in `gpuRadialSensePrepGadget`:
| Parameter | Value | Meaning |
|-----------|-------|---------|
| `mode` | 4 | ARKS custom angle mode |
| `deviceno` | 1 | GPU 1 (not GPU 0) |
| `profiles_per_frame` | 32 | 32 spokes per undersampled frame |
| `rotations_per_reconstruction` | 32 | Reconstruct 32 frames at once |
| `buffer_frames_per_rotation` | 32 | CSM buffer holds 32 frames |
| `buffer_length_in_rotations` | 1 | 1 buffer cycle |
| `reconstruction_os_factor_x/y` | 1.5 | 1.5× oversampling in recon |
| `buffer_convolution_kernel_width` | 5.5 | NUFFT convolution kernel |
| `buffer_convolution_oversampling_factor` | 1.25 | NUFFT grid oversampling |

So `rotations_per_reconstruction = 32` × `profiles_per_frame = 32` = **1024 spokes total per reconstruction job** sent downstream.

#### Key parameters in `gpuLALMSenseGadget` (LALM = Linearized Alternating Least squares with Momentum):
| Parameter | Value |
|-----------|-------|
| `number_of_iterations` | 20 |
| `lambda` (regularization) | 3 |
| `huber_value` | 1 |
| `coils_per_subset` | 4 |
| `oversampling_factor` | 1.25 |
| `kernel_width` | 5.5 |

Slices 1 and 2 use property inheritance (`value@slice0`) so they share parameters with slice 0 — change slice 0 and the others follow.

#### Commented-out sections:
- A **mode 3** golden ratio config block is commented out — this was the previous working config before mode 4, kept for reference
- `AutoScaleGadget` + `FloatToUShortGadget` are commented out — output stays as float rather than scaled unsigned short
- `ImageWriterGadgetFLOAT` commented out — using `MRIImageWriter` (the generic writer) instead

---

### 2. `README.md` — workflow documentation

Documents the full 7-step workflow:
1. Activate conda environment (`gadgetron`)
2. Build with CMake + Ninja (`-DUSE_MKL=ON`, `Release`)
3. Copy XML to Gadgetron config dir (`/opt/conda/envs/gadgetron/share/gadgetron/config/`)
4. Define config variables
5. Convert Siemens `.dat` → ISMRMRD `.h5` using `siemens_to_ismrmrd -Z`
6. Run noise/dependency scan through `default_measurement_dependencies.xml`
7. Run main reconstruction with the custom XML

The example filenames (`meas_MID00347`, `meas_MID00090_FID08384_...`) confirm this was validated against real scanner data.

---

### Issues to flag

**1. `\n` literal in the XML.** In the active `gpuRadialSensePrepGadget` block, there is a literal `\n` embedded in the closing tag:
```xml
<property>...<value>1.5</value></property>\n    </gadget>
```
This `\n` is a text node, not a real newline. Depending on how strictly the Gadgetron XML parser validates, this may either be silently ignored or cause a parse warning. It should be a real newline.

**2. Pipeline is hardcoded to GPU device 1.** All gadgets use `<deviceno>1</deviceno>`. If the machine only has one GPU (device 0), this will fail at runtime with a CUDA device error. The README does not mention this requirement.

**3. Only 3 slices supported.** Three separate `gpuLALMSenseGadget` instances are hardcoded for slices 0, 1, 2. If the scan has more than 3 slices, data for slice 3+ will be passed through via `pass_on_undesired_data=true` but never reconstructed — silently dropped at `ImageFinishGadget`.

**4. README is missing a trailing newline** — minor, but the file ends without one (same as other files in the repo).

---

### Summary

This commit is the **first complete, documented, working pipeline** for ARKS mode 4 reconstruction. It confirms the approach validated on `meas_FID08384` — 32 spokes/frame, 32-frame batches, LALM iterative solver, GPU device 1, 3 slices.