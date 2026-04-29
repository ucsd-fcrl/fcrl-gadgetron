
---

## Commit `17f5a12` — Replace estimate_dcw with compute_radial_dcw_golden_ratio_2d (analytical) for ARKS merged frames; always recompute DCW in fallback path to fix cuNDArray dimension mismatch crash
**Date:** Apr 16, 2026 (~8 days after the previous commit)

1 file changed, 8 insertions, 6 deletions. Small but critical. Fixes two bugs introduced in commit `2fe0601`: an unstable DCW computation for ARKS frames, and a hard crash when a non-ARKS frame follows an ARKS frame. Both bugs were discovered by running the pipeline on real scan data.

---

## Section 1 — Bug 1: `estimate_dcw` was unstable for ARKS angular distributions

### What `estimate_dcw` does

`Gadgetron::estimate_dcw<float, 2>` is an iterative Pipe-Menon sampling density compensation estimator. It works by:
1. Uploading the trajectory to GPU
2. Convolving each trajectory point onto a grid and back (NUFFT round-trip)
3. Iterating until the ratio of grid density to actual sample density converges

**Requirement:** the algorithm assumes the trajectory samples are distributed reasonably uniformly across k-space. For golden ratio or fixed-angle acquisitions this holds well.

**Why it failed for ARKS:** An ARKS combined frame mixes spokes from two sources:
- Current window: 32 spokes from the last frame (near-golden ratio spacing)
- Historical window: 9–27 spokes clustered around specific lag targets (TR offsets set by the scanner)

The result is an angularly clustered distribution — dense in a few regions, sparse elsewhere. The Pipe-Menon iteration diverges or produces extreme weight values for such distributions, leading to incorrect or corrupted images.

### The replacement: `compute_radial_dcw_golden_ratio_2d` (analytical)

```cpp
// Before:
cuNDArray<floatd2> cu_traj(*combined_traj);
uint64d2 matrix_size(image_dimensions_recon_[0], image_dimensions_recon_[1]);
std::shared_ptr< cuNDArray<float> > cu_dcw = Gadgetron::estimate_dcw<float, 2>(
    cu_traj, matrix_size, oversampling_factor_, 10, kernel_width_);
host_weights_recon_[set*slices_+slice] = *(cu_dcw->to_host());

// After:
host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_golden_ratio_2d<float>(
    samples_per_profile_, total_profiles, oversampling_factor_,
    1.0f / (float(samples_per_profile_) / float(image_dimensions_recon_[0])),
    0, GR_SMALLEST)->to_host();
```

`compute_radial_dcw_golden_ratio_2d` is the **analytical** golden ratio DCW formula used for all mode 2/3/4 standard reconstructions. It does not look at the actual trajectory — it computes weights purely from the expected angular density of a golden ratio acquisition with `total_profiles` spokes.

### Parameter-by-parameter comparison with the existing standard DCW call

The standard DCW call (inside `calculate_density_compensation_for_reconstruction`, mode 4 branch):

```cpp
host_weights_recon_[set*slices_+slice] = *compute_radial_dcw_golden_ratio_2d<float>(
    samples_per_profile_, profiles_per_frame_[set*slices_+slice], oversampling_factor_,
    1.0f / (float(samples_per_profile_) / float(image_dimensions_recon_[0])),
    0, GR_SMALLEST)->to_host();
```

| Parameter | Standard frame | ARKS merged frame | Difference |
|-----------|---------------|-------------------|------------|
| `samples_per_profile_` | same | same | none |
| profile count | `profiles_per_frame_` (32) | `total_profiles` (41–59) | **different** — ARKS has more spokes |
| `oversampling_factor_` | same (1.25) | same | none |
| 4th param (scale) | `1.0f / (spp / recon_dim)` | same formula | none |
| profile offset | `0` | `0` | none |
| GR variant | `GR_SMALLEST` | `GR_SMALLEST` | none |

The only difference is the profile count. A larger `total_profiles` means more spokes are modeled as covering k-space, so the analytical DCW assigns **lower weight per spoke** (each spoke contributes less to total density). This is physically appropriate — more spokes genuinely provide higher coverage density.

### What this approximation sacrifices

The analytical formula assumes spokes are distributed according to the golden ratio. ARKS combined frames are not. As a result:
- Spokes in the clustered historical windows may be **over-weighted** (formula thinks they are spread out, but they are actually dense in a narrow angular range)
- Gaps between windows may be **under-weighted** (under-sampled regions get the same weight as well-sampled ones)

However, this is a stable, bounded approximation — no divergence, no extreme values. The commit message documents the trade-off: "stable for non-uniform angular distributions."

### What this removes

- 1 GPU-to-CPU trajectory upload (`cuNDArray<floatd2> cu_traj(...)`)
- 1 `estimate_dcw` call (10 iterations on GPU, plus internal NUFFT operations)
- 1 `cu_dcw->to_host()` download
- The `#include "cuSDC.h"` is no longer needed functionally — but it is **not removed** from the header in this commit, so no compile change

---

## Section 2 — Bug 2: dimension mismatch crash in the fallback path

### How the crash happened

Consider two consecutive frames for the same (set, slice):

| Frame | ARKS gathered spokes | `host_traj_recon_` size | `host_weights_recon_` size |
|-------|---------------------|------------------------|---------------------------|
| Frame N | 27 gathered → 59 total | `59 × spp` | `59 × spp` ← set by ARKS path |
| Frame N+1 | 0 gathered (lags missing) | `32 × spp` ← updated by fallback trajectory | `59 × spp` ← **stale from Frame N** |

Frame N+1 enters the fallback path (`arks_merged = false`). Before this fix, the fallback only called `calculate_trajectory_for_reconstruction` — which updates `host_traj_recon_` to `32 × spp` — but did **not** call `calculate_density_compensation_for_reconstruction`. So `host_weights_recon_` retained the 59-spoke size from the ARKS merge.

The `GenericReconJob` packages both arrays and sends them to `gpuLALMSenseGadget`. When that gadget applies DCW as a per-sample weight vector, it tries an element-wise operation between a `[32 × spp, ncoils]` data array and a `[59 × spp]` DCW array. Size mismatch → `cuNDArray operator` crash.

### The fix

```cpp
// Normal trajectory/DCW path (non-ARKS or no gathered spokes)
if (!arks_merged) {
    if (mode_ == 2 || mode_ == 3 || mode_ == 4 || rotations_per_reconstruction_ == 0) {
        calculate_trajectory_for_reconstruction(
            profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0),
            set, slice);
    }
    // Always recompute DCW to match trajectory dimensions
    // (ARKS frames may have changed host_weights_recon_ to a different size)
    calculate_density_compensation_for_reconstruction(set, slice);  // ← added
}
```

`calculate_density_compensation_for_reconstruction` is now called unconditionally in the fallback path, regardless of whether the trajectory was recalculated. For mode 4 it calls `compute_radial_dcw_golden_ratio_2d` with `profiles_per_frame_` (32) — correctly sizing `host_weights_recon_` to `32 × spp` to match the trajectory.

### Side effect: redundant DCW calls in some cases

For a standard non-ARKS frame that follows another standard non-ARKS frame, `calculate_density_compensation_for_reconstruction` was previously called only during `reconfigure()`. Now it is called on every frame in the fallback path, even when the trajectory has not changed.

- **For modes 2/3/4 with golden ratio:** the trajectory changes every frame (different `profile_offset`), but the DCW formula only depends on `profiles_per_frame_` and `samples_per_profile_` — it does not use `profile_offset`. So recomputing gives the identical result every frame. This is wasteful but not wrong.
- **For mode 0/1:** the trajectory does not change frame-to-frame, and neither does the DCW. Also wasteful but correct.

The overhead is one `compute_radial_dcw_golden_ratio_2d` call per fallback frame — an analytical GPU computation that is fast compared to the NUFFT reconstruction.

---

## Section 3 — Issues to flag

| # | Issue | Location | Risk |
|---|-------|----------|------|
| 1 | Golden ratio DCW is still approximate for ARKS angular distributions | ARKS merged DCW path | Medium — analytical formula doesn't model actual spoke clustering; over-weights clustered regions, under-weights gaps |
| 2 | `#include "cuSDC.h"` left in header even though `estimate_dcw` is removed | `gpuRadialPrepGadget.h` | Low — unused include, adds unnecessary compile dependency |
| 3 | `calculate_density_compensation_for_reconstruction` is now called every fallback frame, even when DCW has not changed | fallback path | Low — redundant GPU work, but fast and correct |
| 4 | `max_spokes_per_frame_` still not used as an upper bound | ARKS path | Low — still no cap; gathering can return more spokes than `max_spokes_per_frame` |
| 5 | `pdata->get_size(0)` layout assumption for historical spokes (from commit `2fe0601`) | Stage 3 data copy | **High** — not changed in this commit, still unresolved |
| 6 | Overlapping lag window deduplication (from commit `2fe0601`) | `arks_gather_spokes()` | **High** — not changed in this commit, still unresolved |
| 7 | Per-spoke `GDEBUG` noise from commit `83ffcc86` | `process()` | Medium — still not cleaned up |

---

## Summary

Two bugs from commit `2fe0601` are fixed:

1. **`estimate_dcw` replaced** with `compute_radial_dcw_golden_ratio_2d` for ARKS merged frames. The iterative estimator was diverging for clustered angular distributions; the analytical formula is stable even though it is an approximation. The 4th parameter (readout oversampling scale) and the `GR_SMALLEST` variant are identical to the existing standard DCW calls — only the profile count (`total_profiles` vs `profiles_per_frame_`) differs.

2. **Fallback path now always recomputes DCW** via `calculate_density_compensation_for_reconstruction`. Previously, a standard frame following an ARKS frame inherited a stale oversized `host_weights_recon_`, causing a `cuNDArray` dimension mismatch crash in the downstream LALM gadget. The fix adds one unconditional DCW call in the fallback path.
