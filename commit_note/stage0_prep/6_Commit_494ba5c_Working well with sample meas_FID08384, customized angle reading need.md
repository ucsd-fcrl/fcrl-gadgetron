

## Commit `494ba5c2` — Working well with sample meas_FID08384, customized angle reading need to be saved in user_int_1
**Date:** Mar 6, 2026

This is a **major architectural pivot** — the angle source moves from an external CSV file to the ISMRMRD acquisition header itself. The CSV infrastructure is fully deleted and replaced with real-time reading from `user_int[1]` as each spoke arrives.

---

### The core change: CSV → `user_int[1]`

**Before:** angles were pre-loaded from a CSV file at `process_config()` time, stored in a vector, then looked up by index.

**After:** each spoke's angle is read in `process()` as it arrives:
```cpp
float angle_rad = (float)m1->getObjectPtr()->user_int[1] / 10000.0f;
```

The scanner encodes the spoke angle as an integer by multiplying by 10,000 (fixed-point encoding). This is decoded back to radians and appended to `fcrl_custom_angles_rad[]` in arrival order.

The angle is also wrapped to `[0, 2π)`:
```cpp
angle_rad = fmod(angle_rad, 2.0 * M_PI);
if (angle_rad < 0) angle_rad += 2.0 * M_PI;
```

---

### What was removed

- `fcrl_angles_csv_path` XML property — no longer needed
- `fcrl_angles_csv_path_` member variable
- `fcrl_load_custom_angles_from_csv()` — the entire ~80 line CSV parsing function deleted
- `#include <fstream>` and `#include <sstream>` — no longer needed

---

### What stays the same

- `fcrl_custom_angles_rad[]` vector — still populated, just now filled incrementally per spoke instead of upfront
- `fcrl_get_custom_angle(acq_index)` — unchanged, still looks up by index into that vector
- `fcrl_compute_custom_radial_trajectory_2d()` — unchanged
- `fcrl_use_custom_angles` — now simply set to `(mode_ == 4)` at config time, no more conditional on CSV load success

---

### Behavioral difference: angles now grow dynamically

Because angles are appended per-spoke, the vector grows as the scan runs. The index into the vector is the **global acquisition counter** (`fcrl_total_angles - 1` for the current spoke). When `fcrl_get_custom_angle(acq_index)` is called to build a trajectory, it accesses earlier entries from the same vector.

This means **trajectory computation relies on angles already having been stored** from prior spokes. The ordering must be:
1. Spoke arrives → angle appended to `fcrl_custom_angles_rad[]`
2. Later, `calculate_trajectory_for_reconstruction(profile_offset, ...)` looks up angles `[first_profile ... first_profile + N]`

As long as `first_profile_index` never exceeds the number of spokes received so far, this is safe.

---

### Issues to flag

**1. The angle vector grows forever and is never pruned.** Over a long scan, `fcrl_custom_angles_rad` accumulates one entry per spoke for the entire scan lifetime. For a 10-minute cardiac scan at ~50 spokes/sec that's ~30,000 entries — small in memory but worth knowing. There is no windowing or clearing.

**2. Fixed-point scale of 10,000 is an implicit contract with the scanner.** The encoding `angle * 10000` stored in `user_int[1]` must be set correctly by the pulse sequence. If a different sequence uses `user_int[1]` for something else, or uses a different scale factor, the angles will be silently wrong. There is no validation that the decoded value is a plausible angle.

**3. Angle wrapping discards rotation information.** After `fmod(..., 2π)`, angles that differ by `2π` become identical. This is fine for trajectory computation (angles are periodic) but means the stored `fcrl_custom_angles_deg` values are also wrapped — if you ever want to reconstruct the original unwrapped angle sequence for analysis, it is lost.

**4. The fallback in `fcrl_get_custom_angle()` is still wrong for out-of-range access.** If `acq_index >= fcrl_total_angles` (i.e., the trajectory function asks for an angle that hasn't arrived yet), it falls back to `acq_index * GR_SMALLEST`. This would silently corrupt the trajectory. This shouldn't happen in normal operation but there's no assertion or error log for this case.

**5. Debug logging in `process()` from the previous commit is still present.** The per-spoke `GDEBUG` lines (process call count, frame counter, recon queue size) were not removed.

---

### Summary table

| Change | Effect |
|--------|--------|
| CSV loading removed | Cleaner — no external file dependency |
| `user_int[1] / 10000.0` per spoke | Angles now embedded in scan data — self-contained |
| `fcrl_use_custom_angles = (mode_ == 4)` | No more silent fallback to golden ratio on CSV failure |
| Angle vector grows per spoke | Memory grows with scan length, never pruned |
| No scale validation | Wrong `user_int[1]` encoding = silent bad angles |

The commit message note — *"customized angle reading need to be saved in user_int_1"* — confirms this was validated against a real scan (`meas_FID08384`) and the approach works, but the pulse sequence must be set up correctly to write angles into `user_int[1]` with the `×10000` fixed-point encoding.