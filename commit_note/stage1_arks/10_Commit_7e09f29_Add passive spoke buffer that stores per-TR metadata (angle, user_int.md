
---

## Commit `7e09f298` — Add passive spoke buffer that stores per-TR metadata (angle, user_int, k-space data) in a sliding window keyed by TR index
**Date:** Apr 1, 2026 (~22 hours after the previous commit)

3 files changed, 64 insertions total. This commit is the first C++ implementation of the ARKS data structure. It adds a passive sliding-window spoke buffer to `gpuRadialPrepGadget` — passive meaning it only accumulates data without yet using it for reconstruction. The reconstruction side is left for the next commit.

Also fixes the highest-priority bug flagged from commit `11a08207`: `fcrl_custom_angle_user_int_index=2` is now present in the ARKS XML.

---

## Section 1 — XML fix (`fcrl_golden_radial_mode4_arks_realtime.xml`)

One line added:
```xml
<property><name>fcrl_custom_angle_user_int_index</name><value>2</value></property>
```

This directly resolves the critical omission from commit `11a08207`. Without it, the gadget defaulted to reading angles from `user_int[1]`, which is the wrong slot — the scanner writes angles into `user_int[2]`. All ARKS reconstructions using `arks_realtime.xml` before this commit would have received garbage angles silently.

---

## Section 2 — New XML properties (`gpuRadialPrepGadget.h`)

Two new `GADGET_PROPERTY` declarations, placed below the existing `fcrl_custom_angle_user_int_index` property:

```cpp
GADGET_PROPERTY(buffer_length_TRs, int, "ARKS: spoke buffer depth in TR units (0=disabled)", 0);
GADGET_PROPERTY(max_spokes_per_frame, int, "ARKS: max spokes per recon frame", 256);
```

### `buffer_length_TRs`
| Detail | Value |
|--------|-------|
| Type | `int` |
| Default | `0` (buffer disabled) |
| XML value in `arks_realtime.xml` | `5000` |
| Controls | How many past TRs of spoke history to retain |

At 5000 TRs and ~50 spokes/sec, this covers ~100 seconds of scan history — enough for multiple cardiac cycles in a retrospective ARKS gating workflow. Setting it to `0` disables the buffer entirely, so the buffer has zero cost in non-ARKS pipelines.

### `max_spokes_per_frame`
| Detail | Value |
|--------|-------|
| Type | `int` |
| Default | `256` |
| XML value in `arks_realtime.xml` | `256` |
| Controls | Maximum spokes allowed per reconstructed ARKS frame |

256 is 8× the standard `profiles_per_frame=32`. This allows ARKS frames to draw from a much wider angular history than a standard undersampled frame, enabling better k-space coverage by aggregating spokes from similar cardiac phases across multiple heartbeats.

**Note:** `max_spokes_per_frame` is stored and initialized in this commit but **never used** in any logic. It is forward-declared for the upcoming bin-selection / reconstruction step. The eviction logic only uses `buffer_length_TRs`.

---

## Section 3 — New `#include`

```cpp
#include <deque>
```

Added to `gpuRadialPrepGadget.h`. Required for `std::deque<ArksSpoke>` in the spoke buffer map. The existing `#include <queue>` was already present for `frame_profiles_queue_` and `recon_profiles_queue_`.

---

## Section 4 — `struct ArksSpoke` (new data structure, `gpuRadialPrepGadget.h`)

```cpp
struct ArksSpoke {
    long tr_index;
    float angle_rad;
    int32_t user_int[ISMRMRD::ISMRMRD_USER_INTS];
    unsigned int slice;
    unsigned int set;
    std::unique_ptr<ProfileMessage> data;
};
```

This is the single unit of storage for one spoke in the ARKS buffer. Every field is explained below.

### `tr_index`
| Type | `long` |
|------|--------|
| Set from | `profiles_counter_global_[buf_key]` **before** the counter increments |
| Purpose | Identifies when this spoke arrived, used for sliding-window eviction |

`profiles_counter_global_[buf_key]` is a per-(set,slice) counter that starts at 0 and increments by 1 at the very end of every `process()` call. Because the ARKS block reads it before the increment, `tr_index` is 0-based and equals the spoke's arrival ordinal for its set/slice pair. TR index 0 = first spoke, 4999 = 5000th spoke, etc.

**Important ordering dependency:** The ARKS block at line ~309 reads `profiles_counter_global_[buf_key]` before the increment at line ~667. Any future refactor that moves the increment earlier in `process()` would silently cause `tr_index` to be off-by-one for every spoke.

### `angle_rad`
| Type | `float` |
|------|---------|
| Set from | `fcrl_custom_angles_rad.back()` |
| Unit | Radians, wrapped to `[0, 2π)` |

`fcrl_custom_angles_rad.back()` is always valid here because the FCRL angle-decode block (lines ~293–300) runs earlier in the same `process()` call and pushes the current spoke's angle before the ARKS block runs. This sequencing is correct — but it means `ArksSpoke.angle_rad` is always the angle just decoded from `user_int[fcrl_custom_angle_user_int_index_]`, not independently re-read.

**Limitation:** `fcrl_custom_angles_rad` is a single flat vector for the whole gadget, not keyed per (set, slice). If the scanner interleaves spokes from different slices, the vector grows with all of them mixed together. `.back()` still returns the current spoke's angle (because this block runs in the same call), but the `fcrl_total_angles` count and any index-based lookup via `fcrl_get_custom_angle(acq_index)` are cross-slice. For single-slice acquisitions this is harmless.

### `user_int[ISMRMRD::ISMRMRD_USER_INTS]`
| Type | `int32_t[8]` |
|------|-------------|
| Set from | `memcpy(spoke.user_int, m1->getObjectPtr()->user_int, sizeof(int32_t) * ISMRMRD::ISMRMRD_USER_INTS)` |
| Purpose | Stores all 8 user_int slots from the acquisition header |

The entire 8-element `user_int` array is deep-copied. At minimum, slot `[fcrl_custom_angle_user_int_index_]` holds the encoded angle. Other slots may carry navigator signals, cardiac phase labels, or other pulse sequence metadata — all preserved for future ARKS bin-selection logic to use.

`ISMRMRD::ISMRMRD_USER_INTS` is a compile-time constant (currently 8). The `memcpy` copies exactly `8 × 4 = 32 bytes`.

### `slice` / `set`
| Type | `unsigned int` |
|------|---------------|
| Set from | `m1->getObjectPtr()->idx.slice` / `m1->getObjectPtr()->idx.set` |
| Purpose | Redundant with `buf_key`, but stored for convenience |

These are also encoded in `buf_key = set * slices_ + slice`, so they can be recovered from the key. Storing them explicitly in the spoke avoids a reverse-computation when iterating the buffer.

### `data`
| Type | `std::unique_ptr<ProfileMessage>` |
|------|----------------------------------|
| Set from | `std::unique_ptr<ProfileMessage>(duplicate_profile(m2))` |
| Purpose | Owns a deep copy of the raw k-space samples for this spoke |

`ProfileMessage` is a type alias defined in the class:
```cpp
using ProfileMessage = GadgetContainerMessage<hoNDArray<std::complex<float>>>;
```

`duplicate_profile(m2)` allocates a new `GadgetContainerMessage` and calls:
```cpp
*copy->getObjectPtr() = *profile->getObjectPtr();
```
This invokes `hoNDArray::operator=`, which performs a deep copy of the complex float sample array. The message's `cont()` (continuation) pointer is **not** copied. For `m2` (the k-space data message), `cont()` is typically null, so this is safe for now.

The `unique_ptr` gives each `ArksSpoke` exclusive ownership of its k-space copy. When a spoke is evicted from the deque (`pop_front()`), the destructor of `ArksSpoke` runs, which calls `~unique_ptr`, which deletes the `ProfileMessage`, which frees the `hoNDArray` allocation. Memory is automatically reclaimed on eviction.

---

## Section 5 — New member variables (`gpuRadialPrepGadget.h`)

```cpp
long arks_buffer_length_TRs_;
long arks_max_spokes_per_frame_;
bool arks_enabled_;
std::map<unsigned int, std::deque<ArksSpoke>> arks_spoke_buffer_;
```

### `arks_buffer_length_TRs_`
Runtime copy of `buffer_length_TRs.value()`. Stored as `long` even though the XML property is `int` — no overflow risk at 5000, but widening is implicit.

### `arks_max_spokes_per_frame_`
Runtime copy of `max_spokes_per_frame.value()`. Stored but not yet used in any logic. Placeholder for the next commit.

### `arks_enabled_`
```cpp
arks_enabled_ = (mode_ == 4 && arks_buffer_length_TRs_ > 0);
```
Single boolean gate. The ARKS block in `process()` is entirely guarded by `if (arks_enabled_)`. If `mode_ != 4` or `buffer_length_TRs_ == 0`, zero overhead — no buffer entries are created, no map keys are touched.

### `arks_spoke_buffer_`
```cpp
std::map<unsigned int, std::deque<ArksSpoke>> arks_spoke_buffer_;
```

| Aspect | Detail |
|--------|--------|
| Key type | `unsigned int` — the flat index `set * slices_ + slice` |
| Value type | `std::deque<ArksSpoke>` |
| Front | Oldest spoke (smallest `tr_index`) |
| Back | Newest spoke (largest `tr_index`) |

`std::deque` is chosen over `std::queue` because deques allow `front()` inspection for eviction checks while still providing O(1) `push_back()` and `pop_front()`. A plain `std::queue` wraps a deque internally but doesn't expose `front().tr_index` directly for range checks.

The map is keyed the same way as `frame_profiles_queue_`, `recon_profiles_queue_`, and `image_headers_queue_` — using `set * slices_ + slice` as a flat 2D index. This is consistent with the rest of the gadget.

**Memory leak risk:** `std::map::operator[]` default-constructs an empty deque on first access. Keys are never removed. If a (set, slice) pair appears in one scan but not the next (within the same Gadgetron process lifetime), its deque stays in the map indefinitely. Each entry holds `unique_ptr<ProfileMessage>` objects that own the k-space data. For single-set, fixed-slice acquisitions this is not a problem.

---

## Section 6 — `process_config()` initialization block

```cpp
arks_buffer_length_TRs_ = buffer_length_TRs.value();
arks_max_spokes_per_frame_ = max_spokes_per_frame.value();
arks_enabled_ = (mode_ == 4 && arks_buffer_length_TRs_ > 0);
arks_spoke_buffer_.clear();

if (arks_enabled_) {
  GDEBUG("ARKS: Spoke buffer enabled. buffer_length_TRs=%ld, max_spokes_per_frame=%ld\n",
         arks_buffer_length_TRs_, arks_max_spokes_per_frame_);
}
```

This block runs once at gadget startup, after the FCRL angle index is validated. Order of operations:

1. `buffer_length_TRs.value()` and `max_spokes_per_frame.value()` — reads from XML properties (or defaults if absent)
2. `arks_enabled_` — evaluated once; `mode_` is already set from XML earlier in `process_config()`
3. `arks_spoke_buffer_.clear()` — ensures no stale data if the gadget is reconfigured mid-session
4. `GDEBUG` — fires only when enabled; logs the two key parameters so a developer can confirm the buffer is active

**No `GADGET_FAIL` on misconfiguration.** If `buffer_length_TRs > 0` but `mode_ != 4`, `arks_enabled_` is silently `false` and the buffer is disabled with no warning. A `GDEBUG` for this case would help catch XML misconfiguration.

---

## Section 7 — `process()` ARKS block

This block runs on every spoke, guarded by `arks_enabled_`. Its position in `process()` is:

```
[noise check + early return]
[FCRL angle decode → fcrl_custom_angles_rad.push_back()]
[ARKS block ← here]
[existing frame detection + queue management]
[profiles_counter_global_[buf_key]++]   ← ARKS reads this BEFORE increment
```

### Step 1 — Compute `buf_key` and `current_tr`

```cpp
unsigned int buf_key = set * slices_ + slice;
long current_tr = profiles_counter_global_[buf_key];
```

`buf_key` is the flat (set, slice) index, consistent with all other per-slice data in the gadget.

`current_tr` is read before the end-of-function increment. So the spoke being inserted gets `tr_index = N` and after `process()` returns, the counter becomes `N+1`. The next spoke will read `N+1`. This is correct: each spoke's TR index equals its 0-based arrival order for its (set, slice) pair.

### Step 2 — Build `ArksSpoke`

```cpp
ArksSpoke spoke;
spoke.tr_index = current_tr;
spoke.angle_rad = fcrl_custom_angles_rad.back();
memcpy(spoke.user_int, m1->getObjectPtr()->user_int, sizeof(int32_t) * ISMRMRD::ISMRMRD_USER_INTS);
spoke.slice = slice;
spoke.set = set;
spoke.data = std::unique_ptr<ProfileMessage>(duplicate_profile(m2));
```

All fields set in one place. The struct is constructed on the stack, then moved into the deque.

### Step 3 — Insert into buffer

```cpp
arks_spoke_buffer_[buf_key].push_back(std::move(spoke));
```

`std::move` transfers ownership of `spoke.data` (the `unique_ptr`) into the deque entry without copying the k-space array. After the move, `spoke.data` is null — but `spoke` goes out of scope immediately after, so this is safe.

### Step 4 — Evict stale spokes

```cpp
auto &buf = arks_spoke_buffer_[buf_key];
while (!buf.empty() && buf.front().tr_index < current_tr - arks_buffer_length_TRs_) {
    buf.pop_front();
}
```

The eviction condition: `buf.front().tr_index < current_tr - arks_buffer_length_TRs_`.

Example at `current_tr = 5000` and `arks_buffer_length_TRs_ = 5000`:
- Threshold = `5000 - 5000 = 0`
- Any spoke with `tr_index < 0` is evicted → nothing, since `tr_index` is always ≥ 0
- The buffer retains TRs `[0, 5000]` — a window of 5001 entries

At `current_tr = 5001`:
- Threshold = `5001 - 5000 = 1`
- Spoke at `tr_index = 0` is evicted
- Buffer retains TRs `[1, 5001]` — exactly 5001 entries

**Ordering note:** The new spoke is inserted in Step 3 before eviction runs in Step 4. The buffer transiently holds one extra entry at the eviction boundary. For a 5000-TR window, peak size is 5001 spokes per (set, slice). Each spoke holds a full k-space copy (`samples_per_profile_ × num_coils` complex floats). At 256 samples, 8 coils, 5001 spokes: `5001 × 256 × 8 × 8 bytes ≈ 82 MB` per slice.

`pop_front()` on a `std::deque` is O(1). The destructor of the evicted `ArksSpoke` runs, which calls `~unique_ptr<ProfileMessage>`, which calls `delete` on the `GadgetContainerMessage`, which frees the `hoNDArray` backing store.

### Step 5 — Periodic logging

```cpp
if (current_tr <= 5 || current_tr % 1000 == 0) {
    long oldest_tr = buf.empty() ? -1 : buf.front().tr_index;
    long newest_tr = buf.empty() ? -1 : buf.back().tr_index;
    size_t buf_size = buf.size();
    GDEBUG("ARKS: [set=%u,slice=%u] buffer_size=%zu, TR_range=[%ld,%ld], current_TR=%ld\n",
           set, slice, buf_size, oldest_tr, newest_tr, current_tr);
}
```

Logs at: the first 6 spokes (TRs 0–5) and then every 1000th TR. At 50 spokes/sec this is once per 20 seconds after warmup — reasonable. This is `GDEBUG` (debug level), so it does not appear in production unless debug logging is enabled.

---

## Section 8 — Issues to flag

| # | Issue | Location | Risk |
|---|-------|----------|------|
| 1 | `arks_max_spokes_per_frame_` stored but never used | `process_config()` + header | Low — intentional forward declaration, but dead code until next commit |
| 2 | Eviction uses `<` not `<=` — buffer window is `arks_buffer_length_TRs_ + 1` entries wide | `process()` eviction loop | Low — off-by-one in window size, likely intentional but undocumented |
| 3 | `arks_spoke_buffer_` keys are never removed | `process()` map access | Low now, medium risk for long-running multi-set scans — each unused key holds a deque with `unique_ptr` entries |
| 4 | `duplicate_profile` does not copy the `cont()` chain | `duplicate_profile()` | Low for now — `m2->cont()` is null for raw k-space messages, but fragile if the message structure changes |
| 5 | No OOM check after `duplicate_profile` | `process()` | Low — `new` throws on failure, so it will throw rather than store null, but the exception is uncaught and will crash the gadget |
| 6 | `arks_enabled_ = false` when `buffer_length_TRs > 0 && mode_ != 4` — silently disabled | `process_config()` | Low — no warning log for this misconfiguration |
| 7 | `fcrl_custom_angles_rad` is flat across all slices | `process()` angle read | Low for single-slice, medium for multi-slice — `.back()` is correct because FCRL block runs first, but `fcrl_total_angles` mixes slices |
| 8 | Per-spoke `GDEBUG` from commit `83ffcc86` still present below the ARKS block | `process()` | Medium — still fires every 32 spokes unconditionally, log noise in production |

---

## Summary

This commit implements the ARKS spoke buffer as a pure accumulation structure with no reconstruction yet. It is passive: every incoming mode-4 spoke is copied into a per-(set,slice) `std::deque<ArksSpoke>`, old spokes are evicted when they age past `buffer_length_TRs`, and the buffer otherwise sits dormant. The XML fix (`fcrl_custom_angle_user_int_index=2`) is the most operationally critical change. The `max_spokes_per_frame` property is initialized but unused — the next commit is expected to implement the bin-selection step that will consume the buffer.
