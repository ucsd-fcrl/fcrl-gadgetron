
## Commit `6c672621` — Custom file input file free, recon angle from h5 file header
**Date:** Feb 27, 2026 (2 days after previous commit)

Despite the commit message mentioning "h5 file header" (which is slightly misleading — no HDF5 reading code was added), this commit is actually a **bug fix and API refinement** to `fcrl_compute_custom_radial_trajectory_2d`. Three concrete problems from the previous commit were addressed.

---

### Change 1: Function signature extended with `num_frames`

**Before:**
```cpp
fcrl_compute_custom_radial_trajectory_2d(
    long num_samples_per_profile, long num_profiles, long first_profile_index)
```

**After:**
```cpp
fcrl_compute_custom_radial_trajectory_2d(
    long num_samples_per_profile, long num_profiles_per_frame, long num_frames, long first_profile_index)
```

The previous version hardcoded `num_frames = 1` and flattened everything into a single dimension. The upstream `compute_radial_trajectory_golden_ratio_2d` returns a 2D array `[samples_per_frame, num_frames]` — this function now correctly matches that layout. All three call sites were updated to pass `num_frames` explicitly.

**Why it matters:** When `rotations_per_reconstruction_ > 0`, you reconstruct multiple frames at once. The previous code was packing all profiles into `dim[0]` with `dim[1] = 1`, which would have produced the wrong array shape for downstream recon — likely causing a dimension mismatch crash or incorrect grid computation in the NUFFT.

---

### Change 2: `+PI` offset added to all angles

Inside the trajectory loop:
```cpp
angle += (float)M_PI;
```

**Why:** The upstream golden ratio CUDA kernel uses the convention:
```
gad_sincos( (profile + offset) * angle_step + PI, ... )
```
The previous commit was missing this `+PI`. Without it, your custom-angle trajectory would be rotated 180° relative to what the downstream `gpuCgSenseGadget` expects, producing a mirrored or phase-shifted image. This is a **correctness fix**.

---

### Change 3: Index layout corrected for multi-frame case

**Before** (flat loop):
```cpp
for (long profile = 0; profile < num_profiles; profile++) {
    size_t idx = sample + profile * num_samples_per_profile;
```

**After** (frame × profile nested loop):
```cpp
for (long frame = 0; frame < num_frames; frame++) {
    for (long profile = 0; profile < num_profiles_per_frame; profile++) {
        long global_profile_idx = first_profile_index + frame * num_profiles_per_frame + profile;
        size_t idx = frame * samples_per_frame + profile * num_samples_per_profile + sample;
```

Global profile index now correctly advances across frames: `first + frame*PPF + profile`. The memory layout now matches `[frame][profile][sample]` which is what the downstream expects.

---

### Change 4: Golden angle fallback value corrected

**Before:**
```cpp
const float small_golden_angle = M_PI * (3.0f - std::sqrt(5.0f));  // ~2.4 rad (~137.5°)
```

**After:**
```cpp
const float small_golden_angle = M_PI * (3.0f - std::sqrt(5.0f)) * 0.5f;  // ~1.2 rad (~68.75°), matches GR_SMALLEST
```

The previous fallback was using the **large golden angle** (~137.5°). `GR_SMALLEST` (what mode 3 and mode 4 use) is half that (~68.75°). The fallback now correctly matches what `compute_radial_trajectory_golden_ratio_2d` produces with `GR_SMALLEST`, so if the CSV runs out of angles mid-scan, the trajectory is continuous rather than jumping to the wrong angular increment.

---

### Summary table

| Change | What was wrong | Fix |
|--------|---------------|-----|
| `num_frames` parameter | Hardcoded to 1, wrong for multi-frame recon | Added as explicit parameter |
| `+PI` offset | Missing, causing 180° rotation vs golden ratio kernel convention | Added `angle += M_PI` |
| Multi-frame index | All profiles packed into frame 0 | Nested `frame × profile` loop with correct global index |
| Fallback golden angle | Used large golden angle (~137.5°) instead of GR_SMALLEST (~68.75°) | Multiplied by 0.5 |

---

### Remaining concern

The commit title says "recon angle from h5 file header" — but no code reads from an HDF5 file. The angle source is still the external CSV file (`user_int_1` column). This suggests the commit message describes the **goal** (eventually read angles embedded in the HDF5 scan data), but what was actually implemented is still the CSV path. The next commits (`494ba5c2`, `2c846c5e`) are likely where the transition to reading `user_int` fields directly from the acquisition header happens.