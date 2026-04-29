
---

## Commit `2fe0601` — Gather historical spokes from TR buffer using lag metadata (user_int[4..7]), merge with current frame, build trajectory and DCW via estimate_dcw
**Date:** Apr 8, 2026 (~7 days after the previous commit)

3 files changed, 242 insertions, 10 deletions. This is the first commit that actually **uses** the spoke buffer built in `7e09f298`. It adds the full ARKS reconstruction pipeline: read lag targets from `user_int[4..7]`, gather historical spokes from the buffer, merge with the current frame, build a combined trajectory, compute DCW iteratively on GPU, and pass the combined k-space job downstream. Also adds file logging infrastructure for debugging.

---

## Section 1 — XML changes (`fcrl_golden_radial_mode4_arks_realtime.xml`)

### `rotations_per_reconstruction` changed from `32` to `0`

```xml
- <property><name>rotations_per_reconstruction</name><value>32</value></property>
+ <property><name>rotations_per_reconstruction</name><value>0</value></property>
```

| Value | Meaning |
|-------|---------|
| `32` (before) | Batch 32 rotations × 32 spokes = 1024 spokes per recon job |
| `0` (now) | Reconstruct one frame at a time (32 spokes per job) |

Frame-by-frame mode is required for ARKS: each frame gets its own custom augmentation set drawn from different parts of the TR buffer. Batching multiple frames into one job is incompatible because each frame has a different set of gathered historical spokes.

### Two new logging properties

```xml
<property><name>arks_log_enabled</name><value>true</value></property>
<property><name>arks_log_file</name><value>/work/arks_log.txt</value></property>
```

These enable the new file-based ARKS reconstruction log (see Section 4). Note: the C++ default for `arks_log_file` is `/tmp/arks_log.txt`, but the XML overrides it to `/work/arks_log.txt`.

---

## Section 2 — New header additions (`gpuRadialPrepGadget.h`)

### New includes

```cpp
#include "cuSDC.h"     // provides estimate_dcw<float, 2>()
#include <cstdio>      // FILE*, fopen, fclose, fprintf
#include <algorithm>   // std::max
```

`cuSDC.h` is the Gadgetron GPU sampling density compensation header. `estimate_dcw` performs iterative Pipe-Menon DCW estimation entirely on the GPU given a non-Cartesian trajectory.

### New GADGET_PROPERTY declarations

```cpp
GADGET_PROPERTY(arks_log_enabled, bool, "ARKS: enable file logging", false);
GADGET_PROPERTY(arks_log_file, std::string, "ARKS: log file path", "/tmp/arks_log.txt");
```

| Property | Type | Default | XML override |
|----------|------|---------|-------------|
| `arks_log_enabled` | bool | `false` | `true` |
| `arks_log_file` | `std::string` | `/tmp/arks_log.txt` | `/work/arks_log.txt` |

---

## Section 3 — `struct ArksGatherResult` (new, `gpuRadialPrepGadget.h`)

```cpp
struct ArksGatherResult {
    std::vector<ProfileMessage*> profiles;  // borrowed pointers (owned by arks_spoke_buffer_)
    std::vector<float> angles_rad;
    size_t total_gathered;                  // == profiles.size()
};
```

The return type of `arks_gather_spokes()`. It is a local struct that lives for the duration of one `process()` call.

### `profiles`
Raw `ProfileMessage*` pointers — **borrowed, not owned**. Each pointer points into a `std::unique_ptr<ProfileMessage>` stored inside `arks_spoke_buffer_`. The caller must copy the data out (via `memcpy`) before the next `process()` call, which may evict entries from the deque and invalidate the pointers.

Safety window: within a single `process()` call, eviction runs during the ARKS buffer-population step (early in the function), and Stage 3 gathering + data copying runs later in the same call. No eviction happens between gathering and copying, so the borrowed pointers are valid throughout Stage 3.

### `angles_rad`
The angle (radians) for each borrowed profile, in the same order as `profiles`. These are values that were already stored in `ArksSpoke.angle_rad` — no re-decoding is needed.

### `total_gathered`
Always equal to `profiles.size()`. Redundant but used as a quick zero-check (`if (gathered.total_gathered > 0)`) to skip the merge when no historical spokes were found.

---

## Section 4 — File logging infrastructure

### Member variable: `FILE* arks_log_fp_`

| Initialized | In constructor, to `nullptr` |
|-------------|------------------------------|
| Opened | In `process_config()` if `arks_enabled_ && arks_log_enabled.value()` |
| Closed | In `~gpuRadialPrepGadget()` |

The destructor is now non-trivial — the previous `~gpuRadialPrepGadget() {}` becomes:
```cpp
gpuRadialPrepGadget::~gpuRadialPrepGadget() {
    if (arks_log_fp_) {
        fclose(arks_log_fp_);
        arks_log_fp_ = nullptr;
    }
}
```

The log file is opened with `"w"` mode — creates or truncates on startup. Each Gadgetron session overwrites the previous log.

### Member function: `arks_log(const char* fmt, ...)`

```cpp
void gpuRadialPrepGadget::arks_log(const char* fmt, ...) {
    if (!arks_log_fp_) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    fprintf(arks_log_fp_, "%02d-%02d %02d:%02d:%02d.%03ld ",
            tm_buf.tm_mon+1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            ts.tv_nsec / 1000000);
    va_list args;
    va_start(args, fmt);
    vfprintf(arks_log_fp_, fmt, args);
    va_end(args);
    fflush(arks_log_fp_);
}
```

Timestamp format: `MM-DD HH:MM:SS.mmm` — millisecond resolution, but **no year**. `fflush` is called on every write, so the log survives a crash at the cost of I/O overhead per reconstruction frame.

This function is called in two places:
- `process_config()` — logs "ARKS log started" with buffer parameters
- Stage 3 in `process()` — logs one line per reconstructed ARKS frame

### `process_config()` — logging initialization block

```cpp
arks_log_fp_ = nullptr;
if (arks_enabled_ && arks_log_enabled.value()) {
    std::string log_path = arks_log_file.value();
    arks_log_fp_ = fopen(log_path.c_str(), "w");
    if (arks_log_fp_) {
        GDEBUG("ARKS: Log file opened: %s\n", log_path.c_str());
        arks_log("ARKS log started. buffer_length_TRs=%ld, max_spokes_per_frame=%ld\n",
                arks_buffer_length_TRs_, arks_max_spokes_per_frame_);
    } else {
        GDEBUG("ARKS: WARNING: Failed to open log file: %s\n", log_path.c_str());
    }
}
```

If `fopen` fails (e.g., `/work/` does not exist or is not writable), `arks_log_fp_` stays `nullptr`, all subsequent `arks_log()` calls return immediately, and the scan continues silently without logging. The warning is `GDEBUG` — invisible in production unless debug logging is enabled.

---

## Section 5 — `arks_gather_spokes()` (new function)

```cpp
ArksGatherResult gpuRadialPrepGadget::arks_gather_spokes(
    long current_tr, unsigned int set, unsigned int slice,
    const int32_t* current_user_int)
```

This function reads the lag protocol from `current_user_int` and collects matching historical spokes from `arks_spoke_buffer_`.

### The lag protocol — `user_int` slot layout

| Slot | Meaning |
|------|---------|
| `user_int[2]` | Spoke angle (encoded as `angle_rad × 10000`) — existing from prior commits |
| `user_int[3]` | `N_samples`: half-window size in TR units |
| `user_int[4]` | Lag 1: TRs back from `current_tr` to first target center |
| `user_int[5]` | Lag 2: TRs back to second target center |
| `user_int[6]` | Lag 3: TRs back to third target center |
| `user_int[7]` | Lag 4: TRs back to fourth target center |

For each active lag slot (value > 0), the gather window is:
```
target_tr  = current_tr - lag
half_window = N_samples / 2
gather_start = target_tr - half_window
gather_end   = target_tr + half_window
```

Example with `N_samples=9` and lags `[100, 200, 300, 400]`:
- Lag 100 → TRs `[current - 104, current - 96]` (9 spokes if all present)
- Lag 200 → TRs `[current - 204, current - 196]`
- Total gathered up to 4 × 9 = 36 spokes from history

### Buffer access and bounds check

```cpp
const auto &buf = it->second;
long buf_oldest_tr = buf.front().tr_index;
long buf_newest_tr = buf.back().tr_index;
```

Each lag window is checked against `[buf_oldest_tr, buf_newest_tr]` before searching:
```cpp
if (gather_end < buf_oldest_tr || gather_start > buf_newest_tr) {
    continue;
}
```
If the lag target falls entirely outside the buffer (too old or too new), the slot is skipped. Windows that partially overlap are clamped:
```cpp
if (gather_start < buf_oldest_tr) gather_start = buf_oldest_tr;
if (gather_end > buf_newest_tr)   gather_end = buf_newest_tr;
```

### Binary search within the deque

```cpp
size_t lo = 0, hi = buf.size();
while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (buf[mid].tr_index < gather_start)
        lo = mid + 1;
    else
        hi = mid;
}
```

Standard lower-bound search. Valid because the deque is monotonically increasing by `tr_index` — spokes are always appended in arrival order and eviction only removes from the front. `std::deque` supports O(1) random access (`buf[mid]`), so each binary search is O(log N) in buffer size.

After the search, a linear scan collects all spokes in `[gather_start, gather_end]` with matching slice and set:
```cpp
for (size_t idx = lo; idx < buf.size() && buf[idx].tr_index <= gather_end; idx++) {
    if (buf[idx].slice == slice && buf[idx].set == set) {
        result.profiles.push_back(buf[idx].data.get());
        result.angles_rad.push_back(buf[idx].angle_rad);
    }
}
```

The slice/set filter is redundant because `buf_key = set * slices_ + slice` already selects the right deque — but it is a safe defensive check that costs nothing.

---

## Section 6 — `arks_build_combined_trajectory_2d()` (new function)

```cpp
boost::shared_ptr< hoNDArray<floatd2> >
gpuRadialPrepGadget::arks_build_combined_trajectory_2d(const std::vector<float>& angles_rad)
```

Builds a flat `[total_profiles × samples_per_profile_, 1]` radial trajectory from an explicit angle list.

### Math

For each profile `p` and sample `s`:
```
angle  = angles_rad[p] + PI         // +PI matches the upstream golden-ratio kernel convention
cos_a  = cos(angle)
sin_a  = sin(angle)
x[s]   = (s - N/2) * cos_a / N
y[s]   = (s - N/2) * sin_a / N
idx    = p * N + s
```

Where `N = samples_per_profile_` and `sample_scale = 1/N`. This is identical to the math in `fcrl_compute_custom_radial_trajectory_2d` (commit `6c67262`), but:

| Difference | `fcrl_compute_custom_radial_trajectory_2d` | `arks_build_combined_trajectory_2d` |
|------------|--------------------------------------------|--------------------------------------|
| Output type | `cuNDArray<floatd2>` (GPU) | `hoNDArray<floatd2>` (host) |
| Multi-frame support | Yes — `[samples_per_frame, num_frames]` | No — always `[total_samples, 1]` |
| Angle source | Indexed lookup from `fcrl_custom_angles_rad` | Passed directly as `std::vector<float>` |

The result stays on host (`hoNDArray`) because it is immediately copied into `host_traj_recon_[set*slices_+slice]` before being uploaded to GPU inside the `GenericReconJob` path.

---

## Section 7 — ARKS Stage 3: merge block in `process()`

This block runs inside the frame reconstruction path, immediately after `extract_samples_from_queue` drains the recon queue into `samples_host`.

### Position in `process()` execution order

```
extract_samples_from_queue → samples_host
ARKS Stage 3 (new block)
  ├── arks_gather_spokes()
  ├── build combined_angles
  ├── build combined data array (memcpy current + historical)
  ├── samples_host = combined          ← replaces the extracted samples
  ├── arks_build_combined_trajectory_2d → host_traj_recon_
  ├── estimate_dcw (GPU) → host_weights_recon_
  └── arks_merged = true
Normal trajectory path (only if !arks_merged)
GenericReconJob ← consumes samples_host, host_traj_recon_, host_weights_recon_
```

### Step 1 — Compute `current_profiles`

```cpp
long current_profiles = (long)recon_profiles_queue_[set*slices_+slice].size();
// recon queue was just emptied, so current_profiles=0; use profiles_per_reconstruction instead
current_profiles = profiles_per_frame_[set*slices_+slice];
if (rotations_per_reconstruction_ > 0)
    current_profiles *= (frames_per_rotation_[set*slices_+slice] * rotations_per_reconstruction_);
```

The comment explains the override: `extract_samples_from_queue` has already emptied the queue, so `.size() == 0`. The actual count is reconstructed from the configuration. With `rotations_per_reconstruction_ = 0`, `current_profiles = profiles_per_frame_ = 32`.

### Step 2 — Build `combined_angles`

```cpp
long first_profile_idx = std::max(0L, profile_offset_arks - current_profiles + 1);
for (long i = 0; i < current_profiles; i++) {
    combined_angles.push_back(fcrl_get_custom_angle(first_profile_idx + i));
}
combined_angles.insert(combined_angles.end(),
                       gathered.angles_rad.begin(), gathered.angles_rad.end());
```

Current window: angles `[first_profile_idx, ..., first_profile_idx + 31]` from `fcrl_custom_angles_rad`.
Historical: angles already stored in `gathered.angles_rad`, appended after the current window.

Order: current window first, gathered spokes second. The data copy in Step 4 must use the same order.

### Step 3 — Allocate combined data array

```cpp
std::vector<size_t> combined_dims = {
    (size_t)(total_profiles * samples_per_profile_), (size_t)ncoils
};
boost::shared_ptr< hoNDArray<float_complext> > combined(new hoNDArray<float_complext>(combined_dims));
memset(combined->get_data_ptr(), 0, combined->get_number_of_bytes());
```

Layout: `[total_profiles × samples_per_profile_, num_coils]` — sample dimension first, coil dimension second. The entire array is zero-initialized before copying.

### Step 4 — Copy current window data

```cpp
for (unsigned int c = 0; c < ncoils; c++) {
    float_complext* src = samples_host->get_data_ptr() + c * samples_per_profile_ * current_profiles;
    float_complext* dst = combined->get_data_ptr() + c * samples_per_profile_ * total_profiles;
    memcpy(dst, src, samples_per_profile_ * current_profiles * sizeof(float_complext));
}
```

`samples_host` layout is assumed to be `[samples × current_profiles, ncoils]` — coil-major, each coil block is `samples_per_profile_ × current_profiles` contiguous. This matches the output of `extract_samples_from_queue`.

The copy writes each coil block starting at offset `c * samples_per_profile_ * total_profiles` in the combined array, leaving the tail (`current_profiles → total_profiles`) zero until filled by historical spokes.

### Step 5 — Copy historical spoke data

```cpp
for (size_t g = 0; g < gathered.profiles.size(); g++) {
    hoNDArray< std::complex<float> >* pdata = gathered.profiles[g]->getObjectPtr();
    for (unsigned int c = 0; c < ncoils; c++) {
        float_complext* dst = combined->get_data_ptr()
            + c * samples_per_profile_ * total_profiles
            + (current_profiles + (long)g) * samples_per_profile_;
        std::complex<float>* src = pdata->get_data_ptr() + c * pdata->get_size(0);
        memcpy(dst, src, samples_per_profile_ * sizeof(float_complext));
    }
}
```

For each gathered spoke `g`, each coil `c` is copied from the stored `ProfileMessage`. 

**Critical assumption:** `pdata->get_size(0) == samples_per_profile_`. This treats dimension 0 of the stored profile as the sample count. If the `hoNDArray` inside `ProfileMessage` has a different layout (e.g., `[samples * coils]` flat, or `[coils, samples]`), the source pointer arithmetic is wrong and the copy silently produces garbage data.

After this step, `samples_host = combined` replaces the extracted current-frame data with the augmented combined array.

### Step 6 — Build combined trajectory and compute DCW

```cpp
boost::shared_ptr< hoNDArray<floatd2> > combined_traj =
    arks_build_combined_trajectory_2d(combined_angles);
host_traj_recon_[set*slices_+slice] = *combined_traj;

cuNDArray<floatd2> cu_traj(*combined_traj);
uint64d2 matrix_size(image_dimensions_recon_[0], image_dimensions_recon_[1]);
std::shared_ptr< cuNDArray<float> > cu_dcw = Gadgetron::estimate_dcw<float, 2>(
    cu_traj, matrix_size, oversampling_factor_, 10, kernel_width_);
host_weights_recon_[set*slices_+slice] = *(cu_dcw->to_host());
```

`estimate_dcw<float, 2>` parameters:
| Parameter | Value | Meaning |
|-----------|-------|---------|
| `cu_traj` | GPU trajectory array | Non-Cartesian sample locations |
| `matrix_size` | `image_dimensions_recon_[0,1]` | Reconstruction grid size |
| `oversampling_factor_` | `1.25` (from XML) | NUFFT grid oversampling |
| `10` | iterations | Number of Pipe-Menon iterations |
| `kernel_width_` | `5.5` (from XML) | Convolution kernel width |

This directly fixes the **DCW correctness bug** first flagged in commit `83ffcc86`. Previous commits used golden-ratio DCW for the ARKS custom-angle trajectory. Now the actual trajectory geometry is used to iteratively estimate the correct density compensation weights.

The result is downloaded from GPU (`cu_dcw->to_host()`) and stored in `host_weights_recon_[set*slices_+slice]` — the same slot that the downstream `GenericReconJob` reads from. No separate memory allocation is needed.

---

## Section 8 — Normal trajectory fallback path

```cpp
if (!arks_merged) {
    if (mode_ == 2 || mode_ == 3 || mode_ == 4 || rotations_per_reconstruction_ == 0) {
        calculate_trajectory_for_reconstruction(
            profiles_counter_global_[set*slices_+slice] - ((new_frame_detected) ? 1 : 0),
            set, slice);
    }
}
```

When ARKS is enabled but `gathered.total_gathered == 0` (e.g., all lag slots are 0 or the buffer is empty), `arks_merged` remains `false` and the normal trajectory calculation runs. This is the correct fallback — no regression for the standard golden-ratio path.

**Note:** DCW is not recalculated in the fallback path. The existing `host_weights_recon_` from `calculate_density_compensation_for_reconstruction` (which ran at configuration time) is reused. This is unchanged behavior.

---

## Section 9 — Issues to flag

| # | Issue | Location | Risk |
|---|-------|----------|------|
| 1 | `arks_max_spokes_per_frame_` still unused | everywhere | Low — no cap on gathered spokes, `max_spokes_per_frame` XML parameter has no effect yet |
| 2 | Overlapping lag windows → duplicate spokes | `arks_gather_spokes()` | **High** — if two lag values are close, spokes at the overlap TRs appear twice in `gathered.profiles`, corrupting both data and trajectory |
| 3 | Lag metadata read from the **last** spoke of the frame (`m1`, profile 31) — see expanded note below | Stage 3 in `process()` | Medium — silently disables ARKS if pulse sequence does not write lags into every spoke |
| 4 | `pdata->get_size(0)` assumed to equal `samples_per_profile_` | historical spoke copy in Stage 3 | **High** — if the stored array layout differs, all historical k-space data is silently misaligned |
| 5 | No check that historical spokes have the same coil count | Stage 3 coil copy loop | Medium — mismatch in coil count would cause out-of-bounds read |
| 6 | `estimate_dcw` called every frame on GPU | Stage 3 | Medium — adds per-frame GPU computation; 10 iterations per frame must fit within the TR budget |
| 7 | `arks_log()` is not thread-safe | `arks_log()` | Low for single-slice, medium for multi-slice parallel — no mutex around `fprintf`/`fflush` |
| 8 | Log timestamp has no year | `arks_log()` | Low — ambiguous for multi-day scans or logs spanning midnight |
| 9 | `arks_log_file` default (`/tmp/`) diverges from XML (`/work/`) | `process_config()` | Low — logs appear in unexpected location if XML property is omitted |
| 10 | `#include <cstdarg>` in `.cpp`, not in `.h` | `gpuRadialPrepGadget.cpp` | Low now — `arks_log` is only called from `.cpp`, but any future caller including just the header would need to add `<cstdarg>` manually |
| 11 | Per-spoke `GDEBUG` from commit `83ffcc86` still present | `process()` | Medium — still unfixed log noise |

---

## Issue 3 — Expanded: which spoke's `user_int` is read for lag metadata

### Where the read happens

```cpp
ArksGatherResult gathered = arks_gather_spokes(
    profile_offset_arks, set, slice,
    m1->getObjectPtr()->user_int);   // ← m1 is the spoke currently in process()
```

`m1` is whichever spoke caused `is_last_profile_in_reconstruction` to become `true`. The question is: which spoke that is.

### Normal execution path (mode 4, `rotations_per_reconstruction_ = 0`)

Profiles arrive in order `0, 1, 2, ..., 31, 0, 1, 2, ..., 31, ...` (each frame has 32 spokes numbered by `kspace_encode_step_1`).

`new_frame_detected` is set when `profile <= previous_profile_` — that is, when spoke 0 arrives after spoke 31. At that point the new spoke is **held back** (not enqueued) until after the reconstruction for the completed frame fires.

The reconstruction gate is:
```cpp
is_last_profile_in_reconstruction = (recon_profiles_queue_.size() == profiles_per_reconstruction);
// = (recon_profiles_queue_.size() == 32)
```

With `rotations_per_reconstruction_ = 0`:
- Spoke 31 arrives → `new_frame_detected = false` → spoke IS enqueued → queue size = 32 → `is_last_profile_in_reconstruction = true` → **reconstruction fires here**
- `m1` at that moment is the spoke with `profile = 31` — the **last spoke of the just-completed frame**

Then spoke 0 (first of the next frame) arrives → `new_frame_detected = true` → queue was already drained to 0 by the reconstruction above → `is_last_profile_in_reconstruction = false` → no second reconstruction

**Conclusion: in normal mode-4 per-frame operation, `m1` at reconstruction time is always the last spoke of the frame (profile index 31).**

### What this means for the pulse sequence contract

`arks_gather_spokes` reads the lag values from `m1->getObjectPtr()->user_int[4..7]`. These lags specify which historical TR windows to gather for the CURRENT frame. The pulse sequence is responsible for writing these values before reconstruction fires.

There are three possible pulse sequence behaviors:

| Behavior | What happens in gadget |
|----------|----------------------|
| Lags written into **every** spoke of the frame | Spoke 31 has correct lags → gather works correctly |
| Lags written only into **spoke 0** of each frame | Spoke 31 has `user_int[4..7] = 0` → all `lag <= 0` → all slots skipped → `total_gathered = 0` → `arks_merged = false` → silent standard recon |
| Lags written into one spoke based on **navigator timing** (e.g., a specific TR when a cardiac trigger fires) | If that spoke is not spoke 31, lags are 0 at reconstruction time → ARKS silently disabled for that frame; may occasionally work if trigger lands on spoke 31 |

The silent failure mode is the danger: `total_gathered = 0` is indistinguishable from a legitimate frame with no valid lag history. The `arks_log` will show `gathered=0` on every frame, which would indicate ARKS is not activating — but only if logging is enabled and the log file is inspected.

### The `new_frame_detected` edge case

In rare cases (dynamic frame count change, recovery from dropped spoke), `new_frame_detected` fires while the queue still holds 32 entries. In that case reconstruction fires with `m1` being the **first spoke of the new frame** (profile = 0). That spoke's `user_int` belongs to the incoming frame, not the completed one. Gathering with these lags selects historical spokes intended for the new frame and merges them with the completed frame's data — a frame mismatch that produces incorrect ARKS augmentation with no error or warning.

### Why this is medium risk, not high

In practice the pulse sequence presumably writes the same lags into all 32 spokes of a frame (the simplest implementation on the scanner side). If so, spoke 31 always has the correct values and the code works correctly. The risk escalates to high only if lags are set per-navigator-event (which is the more realistic physiological implementation). This is a pulse sequence design assumption that is nowhere documented or validated in the gadget code.

---

## Summary

This commit is the first working ARKS reconstruction. The spoke buffer from `7e09f298` is now consumed: lag values from `user_int[4..7]` select historical TR windows, the gathered spokes are merged with the current 32-spoke frame into a combined data array, a new trajectory is built from the explicit angle list, and DCW is computed iteratively via `estimate_dcw` on GPU. The file logger provides per-frame reconstruction tracing for validation. The two most important structural correctness fixes are: (1) the DCW bug from commit `83ffcc86` is finally resolved, and (2) `rotations_per_reconstruction` is correctly set to 0 for per-frame operation.
