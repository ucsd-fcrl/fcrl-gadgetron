Here is the full breakdown of commit `c4e1e9f3`:

---

## Commit `c4e1e9f3` — Making FCRL RadialPRep and SensePrep Gadgets for Testing
**Date:** Feb 4, 2026 (same day as the first commit, ~1 hour later)

This commit created **4 new files** and modified **CMakeLists.txt** to register them into the build. This is the foundational class architecture for all your subsequent ARKS work.

---

### What was built: A 2-class inheritance hierarchy

```
gpuRadialPrepGadgetFCRL          (abstract base, ~900 lines)
        │
        └── gpuRadialSensePrepGadgetFCRL   (concrete, ~90 lines)
```

This mirrors the existing upstream structure:
```
gpuRadialPrepGadget  →  gpuRadialSensePrepGadget  (upstream originals)
gpuRadialPrepGadgetFCRL  →  gpuRadialSensePrepGadgetFCRL  (your FCRL copies)
```

The FCRL versions are **copies of the upstream gadgets**, with the intent to modify them freely for FCRL/ARKS research without touching the original upstream code.

---

### `gpuRadialPrepGadgetFCRL` — the base class

This is a near-copy of the upstream `gpuRadialPrepGadget`. It handles:

**`process_config()`** — reads the XML config and initializes:
- Mode (0/1/2/3), device number, sliding window settings
- Image dimensions (warp-size aligned for GPU)
- Profile/frame counters per slice/set
- Accumulation buffer allocation

**`process()`** — the main per-spoke processing loop:
1. Skips noise profiles
2. Detects frame boundaries by watching `kspace_encode_step_1` roll over
3. Reconfigures automatically if coil count or acceleration factor changes
4. Enqueues each spoke into two separate queues: `frame_profiles_queue_` (for CSM buffer) and `recon_profiles_queue_` (for reconstruction)
5. At the end of each frame: updates the accumulation buffer with `add_frame_data()`
6. When enough profiles are accumulated: computes CSM + regularization image, packages a `GenericReconJob`, and pushes it downstream

**Trajectory/DCW calculation** — four separate methods:
| Method | Purpose |
|--------|---------|
| `calculate_trajectory_for_frame()` | Trajectory for a single undersampled frame going into the CSM buffer |
| `calculate_trajectory_for_rhs()` | Trajectory for the fully sampled buffer (used in iterative recon) |
| `calculate_trajectory_for_reconstruction()` | Trajectory for the downstream recon job |
| `calculate_density_compensation_for_*()` | DCW counterparts for each of the above |

All four methods branch on `mode_`:
- Mode 0/1 → `compute_radial_trajectory_fixed_angle_2d` / `compute_radial_dcw_fixed_angle_2d`
- Mode 2/3 → `compute_radial_trajectory_golden_ratio_2d` / `compute_radial_dcw_golden_ratio_2d`

**Important design note:** `compute_csm()`, `compute_reg()`, `allocate_accumulation_buffer()`, and `get_buffer_ptr()` are **pure virtual** — they must be implemented by the derived class.

---

### `gpuRadialSensePrepGadgetFCRL` — the concrete class

Implements the four pure virtual methods using `cuSenseBuffer` / `cuSenseBufferCg`:

- **`compute_csm()`** — calls `acc_buffer->get_accumulated_coil_images()` then `estimate_b1_map()` to get sensitivity maps, stores them back in the buffer
- **`compute_reg()`** — calls `acc_buffer->get_combined_coil_image()` to get the regularization image; for golden ratio modes with solver enabled, first calls `preprocess()` with the RHS trajectory
- **`allocate_accumulation_buffer()`** — allocates either `cuSenseBuffer` or `cuSenseBufferCg` depending on `buffer_using_solver_`
- **`reconfigure()`** — calls the parent `reconfigure()` then, if solver mode is active, sets DCW for RHS and preprocesses the buffer

---

### `CMakeLists.txt`

Both new files were added to the `gadgetron_gpuradial` shared library and added to the install headers list — so they are built and installed as part of the standard Gadgetron build.

---

### Potential issues to watch for

| Issue | Location | Risk |
|-------|----------|------|
| **Mode 4 is not handled** | All `switch(mode_)` statements only cover cases 0–3, `default` returns `GADGET_FAIL` | If mode 4 is ever passed in, every trajectory/DCW calculation silently fails |
| **`calculate_trajectory_for_frame()` has no return on default** | `gpuRadialPrepGadgetFCRL.cpp:706` — returns an empty `result` (null ptr) rather than failing loudly | If mode is invalid, null trajectory is passed to `add_frame_data()` — likely crash |
| **Profile queues are never size-checked before reconstruction** | The `image_headers_queue_` size IS checked (line 520), but `recon_profiles_queue_` size is trusted implicitly | Could cause misaligned recon if profiles and headers get out of sync |
| **`reconfigure_` flag is never set back to `true` after a mode change** | Only coil count changes and new frame detections trigger reconfiguration checks — mode changes from the XML are a one-time setup | Not a bug here since mode is fixed at config time, but worth knowing |

This commit established the scaffold that all later ARKS commits build on top of.