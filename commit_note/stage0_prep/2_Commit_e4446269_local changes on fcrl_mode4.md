
---

## Commit `e4446269` — WIP: local changes on fcrl_mode4
**Date:** Feb 17, 2026 (~2 weeks after the previous commit)

---

### 1. `build-images.sh` — deleted entirely

The upstream Docker build script for building `dev`/`rt` × `cuda`/`nocuda` image variants was removed. This is a purely **housekeeping deletion** — it had no relation to the FCRL/ARKS recon work and was likely removed to reduce clutter or because it was causing confusion in your local workflow. It has no effect on the gadget behavior.

**Risk:** If you ever need to rebuild Gadgetron Docker images from scratch, this script is gone. You would need to recover it from the upstream repo.

---

### 2. `test/mri_core_stream_test.cpp` — test deserialization path simplified

This change modifies how the unit test reads back streamed array data. Both reads (undersampled k-space and reference k-space) were updated the same way:

**Before:**
```cpp
// Two-step: deserialize ISMRMRD NDArray, then convert to hoNDArray
ISMRMRD::IStreamView rs(fd);
ISMRMRD::ProtocolDeserializer deserializer(rs);
ISMRMRD::NDArray<std::complex<float>> arr;
deserializer.deserialize(arr);

hoNDArray<std::complex<float>> data_deserialized, diff;
Gadgetron::convert_ismrmrd_ndarray_to_hoNDArray(arr, data_deserialized);
```

**After:**
```cpp
// One-step: read directly into hoNDArray using Gadgetron IO
hoNDArray<std::complex<float>> data_deserialized;
Gadgetron::Core::IO::read(fd, data_deserialized);

hoNDArray<std::complex<float>> diff;
```

**Why this matters:** This is directly connected to the `stream_to_array_buffer()` disable in commit `b564bb7e`. The original test was verifying that data written via `stream_to_array_buffer()` (which uses `ISMRMRD::ProtocolSerializer`) could be read back via `ISMRMRD::ProtocolDeserializer`. Since `stream_to_array_buffer()` was disabled and replaced with a no-op, the write path changed — now data is written via `Gadgetron::Core::IO::write()`, so the test read path was updated to match using `Gadgetron::Core::IO::read()`.

**Risk:** The test is now testing a **different serialization format** than what the original `stream_to_array_buffer()` would have produced. If `stream_to_array_buffer()` is ever re-enabled in its original form, this test would silently pass with the wrong deserialization and would not actually validate the round-trip correctly.

---

## Summary

| File | Change | Risk |
|------|--------|------|
| `build-images.sh` | Deleted Docker build helper | Low — recoverable from upstream |
| `mri_core_stream_test.cpp` | Switched test deserialization from ISMRMRD protocol to `Gadgetron::Core::IO::read` | Low now, but creates a consistency gap if `stream_to_array_buffer` is ever restored |

This commit is mostly cleanup / keeping test code consistent with the disabled streaming path from commit 1.