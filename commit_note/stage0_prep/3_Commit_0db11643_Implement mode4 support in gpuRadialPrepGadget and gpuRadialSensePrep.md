
---

## Commit `0db11643` — Implement mode4 support in gpuRadialPrepGadget and gpuRadialSensePrepGadget
**Date:** Feb 18, 2026

Small but important commit — only 15 lines changed across 5 files. It does one conceptual thing: **makes mode 4 an alias for the golden-ratio trajectory path**.

---

### What changed

#### `gpuRadialPrepGadget.cpp` and `gpuRadialPrepGadgetFCRL.cpp` — identical changes in both files

`case 4:` was added as a fall-through on top of `case 3:` in all 6 trajectory/DCW switch statements:

| Method | Change |
|--------|--------|
| `calculate_trajectory_for_reconstruction()` | mode 4 falls through to golden ratio path |
| `calculate_density_compensation_for_reconstruction()` | mode 4 falls through to golden ratio path |
| `calculate_trajectory_for_frame()` | mode 4 falls through to golden ratio path |
| `calculate_density_compensation_for_frame()` | mode 4 falls through to golden ratio path |
| `calculate_trajectory_for_rhs()` | mode 4 falls through to golden ratio path |
| `calculate_density_compensation_for_rhs()` | mode 4 falls through to golden ratio path |

So **mode 4 is identical to mode 3** in terms of trajectory and DCW calculation — both use `GR_SMALLEST` golden ratio. The only reason to add mode 4 as a separate value is to distinguish it at a higher level (e.g., in `process()` logic or in the XML config) for future branching.

#### `gpuRadialPrepGadget.h` and `gpuRadialPrepGadgetFCRL.h`

```diff
- GADGET_PROPERTY_LIMITS(mode,int,"Radial mode", 0, GadgetPropertyLimitsEnumeration, 0,1,2,3);
+ GADGET_PROPERTY_LIMITS(mode,int,"Radial mode", 0, GadgetPropertyLimitsEnumeration, 0,1,2,3,4);
```

Mode 4 is now a **valid accepted value** in the XML config. Without this, passing `<value>4</value>` in the config file would be rejected at `process_config()` time.

#### `gpuRadialSensePrepGadget.h`

```diff
+ #include "gpuRadialPrepGadgetFCRL.h"
```

A curious addition — the upstream `gpuRadialSensePrepGadget.h` (not the FCRL version) now includes the FCRL header. This is unusual and likely unintentional. The upstream class does not use anything from the FCRL header directly.

---

### Issues to flag

**1. Mode 4 has no unique behavior yet.** It is purely an alias for mode 3 (`GR_SMALLEST`). The commit message says "implement mode4 support" but mode 4 does nothing that mode 3 doesn't already do at this point. The differentiation must come in a later commit (likely in the `process()` loop or custom angle handling). If you ever debug a mode 4 recon and it behaves like mode 3, this is why.

**2. `gpuRadialSensePrepGadget.h` includes `gpuRadialPrepGadgetFCRL.h`.** The upstream file should not depend on your custom FCRL header. This creates an unintended coupling — if the FCRL header ever has a compile error, it will also break the upstream `gpuRadialSensePrepGadget`. This looks like it was added by accident and should probably be removed.

**3. `process()` was not updated for mode 4.** Inside `process()`, several checks still only handle modes 0–3 (e.g., the golden ratio branch `if( mode_ == 2 || mode_ == 3 )` in `reconfigure()` of `gpuRadialSensePrepGadgetFCRL.cpp`). Mode 4 would be excluded from those branches. This is a latent bug that gets addressed in later commits.

---

### Summary

| File | Change | Risk |
|------|--------|------|
| `gpuRadialPrepGadget.cpp` | mode 4 falls through to golden ratio in all 6 trajectory/DCW methods | Low — correct behavior as long as mode 4 == mode 3 is intentional |
| `gpuRadialPrepGadgetFCRL.cpp` | Same as above | Low |
| `gpuRadialPrepGadget.h` | mode 4 added to valid property enum | Low |
| `gpuRadialPrepGadgetFCRL.h` | Same as above | Low |
| `gpuRadialSensePrepGadget.h` | Spurious FCRL header include | **Medium** — unintended coupling between upstream and FCRL code |