
Here is the full report:

---

## Commit `83ffcc86` — add mode4 and pipeline, recon image with non-golden angle (ARKS angle)
**Date:** Feb 25, 2026

This is the most significant commit so far. It gives mode 4 its **own distinct identity** — loading spoke angles from an external CSV file instead of using the golden ratio. The same changes are applied in parallel to both `gpuRadialPrepGadget` (upstream) and `gpuRadialPrepGadgetFCRL` (your FCRL copy), so both classes are identical in behavior.

---

### Architecture of the new mode 4 pipeline

```
XML config: <mode>4</mode> + <fcrl_angles_csv_path>/path/to/angles.csv</fcrl_angles_csv_path>
                │
                ▼
       process_config()
       fcrl_load_custom_angles_from_csv()
         → reads "user_int_1" column as angle_deg
         → converts to radians, stores in fcrl_custom_angles_rad[]
                │
                ▼
       process() per spoke
       calculate_trajectory_for_*()
       fcrl_compute_custom_radial_trajectory_2d(samples, profiles, first_idx)
         → looks up angle[first_idx + profile] from fcrl_custom_angles_rad[]
         → builds floatd2 trajectory array matching golden ratio layout
         → uploads to GPU as cuNDArray<floatd2>
```

---

### Change-by-change breakdown

#### 1. New config property: `fcrl_angles_csv_path`
A new XML gadget property is added to both `.h` files:
```xml
<property><name>fcrl_angles_csv_path</name><value>/path/to/angles.csv</value></property>
```
At `process_config()` time, if mode 4 and the path is non-empty, `fcrl_load_custom_angles_from_csv()` is called. If that fails, it falls back silently to golden angle — **no hard failure**.

#### 2. `fcrl_load_custom_angles_from_csv()`
- Opens the CSV, reads the header line to find the column named `user_int_1`
- For each data row, extracts the value at that column as a float (degrees)
- Converts to radians and stores in `fcrl_custom_angles_rad[]`
- Returns `GADGET_FAIL` if the column is missing or no angles were loaded

#### 3. `fcrl_get_custom_angle(long acq_index)`
- Returns `fcrl_custom_angles_rad[acq_index]` if in range
- **Falls back to the small golden angle formula** if index is out of bounds: `acq_index * π*(3−√5)`

#### 4. `fcrl_compute_custom_radial_trajectory_2d()`
Builds the spoke trajectory array by:
- Allocating a `hoNDArray<floatd2>` with shape `[samples_per_profile * num_profiles, 1]`
- For each profile: looks up its angle, computes `(sample - N/2) * cos/sin(angle) / N` for each sample
- Copies to GPU as `cuNDArray<floatd2>`

The coordinate layout intentionally matches `compute_radial_trajectory_golden_ratio_2d` so downstream reconstruction is unaffected.

#### 5. `calculate_trajectory_for_reconstruction()` — mode 4 split out
Mode 4 is now its own `case` block (no longer falls through to mode 3). It calls `fcrl_compute_custom_radial_trajectory_2d()` if custom angles are loaded, otherwise falls back to `GR_SMALLEST` golden ratio.

#### 6. `calculate_trajectory_for_frame()` — same split
Same pattern for the CSM buffer frame trajectory.

#### 7. `calculate_trajectory_for_rhs()` — same split
Same for the iterative solver RHS trajectory.

#### 8. `gpuRadialSensePrepGadget.cpp` and `gpuRadialSensePrepGadgetFCRL.cpp`
Fixed the leftover bug from commit `0db11643`:
```diff
- if( buffer_using_solver_ && ( mode_ == 2 || mode_ == 3 ) ){
+ if( buffer_using_solver_ && ( mode_ == 2 || mode_ == 3 || mode_ == 4 ) ){
```
Mode 4 now correctly preprocesses the CG solver buffer when `buffer_using_solver_` is true.

#### 9. Debug logging added to `process()`
Two persistent `GDEBUG` lines were added:
- Every spoke: logs `profile, counter_frame, per_frame, is_last, new_frame`
- Every spoke: logs `recon_queue_size, profiles_per_recon, is_last_recon, img_queue_size`
- Also logs when an image header is created

**These are verbose and fire on every single spoke.** For a 32-spoke frame acquisition, that's 32 log lines per frame, every frame.

---

### Issues to flag

**1. No DCW for mode 4 custom angles.** `calculate_density_compensation_for_reconstruction()` and `calculate_density_compensation_for_frame()` still fall through to mode 3 (golden ratio DCW). The DCW is computed assuming golden ratio angular distribution, but the actual trajectory uses your custom ARKS angles. If the ARKS angle distribution differs from golden ratio, **the DCW is wrong** — this is likely the root cause of image quality issues at this stage, and explains why later commits switch to `estimate_dcw` and eventually to `compute_radial_dcw_golden_ratio_2d` for ARKS.

**2. `process_call_count` is a `static` local variable.** It persists across all slices, sets, and even multiple scan sessions within the same Gadgetron process lifetime. If Gadgetron reuses the gadget instance across scans, the counter never resets. This is debug code but worth knowing.

**3. Silent CSV fallback.** If the CSV fails to load, the gadget silently continues with golden angle and emits only a `GDEBUG` message. If `GDEBUG` logging is not enabled in your Gadgetron config, this failure is completely invisible. You would get a golden-ratio recon when you expected ARKS angles with no warning.

**4. `fcrl_get_custom_angle()` fallback is inconsistent.** If `acq_index >= fcrl_total_angles` (more spokes than CSV rows), the fallback is `acq_index * small_golden_angle` — an absolute angle, not relative to the last CSV angle. This creates a discontinuity between the last ARKS angle and the first fallback golden angle.

**5. Duplicate code.** The same three helper functions (`fcrl_load_custom_angles_from_csv`, `fcrl_get_custom_angle`, `fcrl_compute_custom_radial_trajectory_2d`) are copy-pasted identically into both `gpuRadialPrepGadget.cpp` and `gpuRadialPrepGadgetFCRL.cpp`. Any future fix to one must be manually applied to the other.

**6. Verbose debug logs left in production path.** The two `GDEBUG` calls inside `process()` fire on every spoke. This was clearly added for debugging and should be guarded by `output_timing_` or removed before production use.

---

### Summary table

| Area | Change | Risk |
|------|--------|------|
| CSV angle loading | New at `process_config` time | Medium — silent fallback on failure |
| Trajectory (recon, frame, rhs) | Mode 4 gets custom angle path | Low — correct layout |
| DCW (all methods) | Still uses golden ratio DCW for mode 4 | **High** — wrong DCW for non-golden angles |
| `compute_reg()` in SensePrep | Mode 4 now included in solver preprocess | Low — correct fix |
| Debug logging in `process()` | Per-spoke GDEBUG on every call | Medium — performance/log noise in production |
| Code duplication | Same helpers in both `.cpp` files | Low now, maintenance risk later |