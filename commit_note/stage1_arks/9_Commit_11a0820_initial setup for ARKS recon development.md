I already have the full diff from before. Let me do the deep analysis now.

Ran terminal command: git -C /home/nickhao/gadgetron/gadgetron_source show 8daec552 -- fcrl_golden_radial_mode4_read_angle_realtime.xml | grep "property"

Ran terminal command: git -C /home/nickhao/gadgetron/gadgetron_source show 11a08207 -- fcrl_golden_radial_mode4_arks_realtime.xml | grep "property"

Ran terminal command:  git -C /home/nickhao/gadgetron/gadgetron_source show 8daec552 -- fcrl_golden_radial_mode4_read_angle_realtime.xml | grep "property"

Ran terminal command:  git -C /home/nickhao/gadgetron/gadgetron_source show 11a08207 -- fcrl_golden_radial_mode4_arks_realtime.xml | grep "property"

---

## Commit `11a08207` — initial setup for ARKS recon development
**Date:** Apr 1, 2026 (~22 hours after the previous commit)

Only one file added: `fcrl_golden_radial_mode4_arks_realtime.xml`. No C++ code changed. But this commit marks a **meaningful pivot** — the word "ARKS" appears in the filename for the first time, and two new properties are declared that don't yet exist in any gadget code.

---

### What this file is

A second pipeline XML, purpose-built for ARKS recon, intended to eventually **replace** `fcrl_golden_radial_mode4_read_angle_realtime.xml`. The gadget chain is structurally identical — same preprocessing (NoiseAdjust → PCA → CoilReduction → gpuRadialSensePrepGadget → 3× gpuLALMSenseGadget → Extract → ImageFinish) — but the `gpuRadialSensePrepGadget` block is meaningfully different.

---

### Side-by-side comparison: old XML vs new XML

| Property | `read_angle_realtime.xml` (prev) | `arks_realtime.xml` (this commit) |
|---|---|---|
| `mode` | 4 | 4 |
| `fcrl_custom_angle_user_int_index` | **2** (explicit) | **missing** → defaults to 1 |
| `profiles_per_frame` | 32 | 32 |
| `rotations_per_reconstruction` | 32 | 32 |
| `buffer_frames_per_rotation` | 32 | 32 |
| `buffer_length_in_rotations` | 1 | 1 |
| `buffer_length_TRs` | missing | **5000** (NEW) |
| `max_spokes_per_frame` | missing | **256** (NEW) |
| Commented-out mode 3 block | yes | **removed** |
| `\n` literal bug | yes | **fixed** |
| AutoScale / FloatToShort blocks | commented out | **removed** |

---

### The two new properties: `buffer_length_TRs` and `max_spokes_per_frame`

These are the most important part of this commit. They do not exist in any gadget C++ code yet at this point — they are being declared in the XML **in anticipation of the spoke buffer** that gets implemented in the next commit (`7e09f298`).

**`buffer_length_TRs = 5000`**
This will control the size of the sliding window TR history buffer. At 5000 TRs, and assuming ~50 spokes/sec, this covers ~100 seconds of scan history — enough to span several cardiac cycles for retrospective ARKS gating.

**`max_spokes_per_frame = 256`**
This caps how many historical spokes can be merged into a single ARKS reconstructed frame. 256 is significantly larger than the standard 32 spokes per frame, allowing ARKS frames to draw from a much wider angular history for better coverage.

Together these two properties define the ARKS spoke buffer design: **hold 5000 TRs of history, allow up to 256 spokes per reconstructed frame**. The downstream LALM gadget is unchanged — it just receives a bigger k-space job.

---

### What changed in the surrounding gadget chain

Everything else is identical between the two XMLs except for cleanup:

- The **commented-out mode 3 fallback block** is gone — this was kept in the previous XML as a reference. Removing it means you committed to mode 4 / ARKS as the only path.
- The **`\n` literal bug** in the closing tag (noted in commit `8daec552`) is gone — the closing `</gadget>` is now a clean newline.
- The **commented-out AutoScale, FloatToShort, and image writer variants** are removed — the output format is settled as float via `MRIImageWriter`.

---

### Issues to flag

**1. `fcrl_custom_angle_user_int_index` is missing.** This defaults to `1` in the C++ code (from commit `2c846c5e`). The previous XML explicitly set it to `2`. If this new XML is used as-is against the same scanner data, it will read angles from `user_int[1]` instead of `user_int[2]` — the wrong slot — and produce a garbage trajectory with no error. This is the most dangerous omission in this commit.

**2. `buffer_length_TRs` and `max_spokes_per_frame` are not yet recognized by the gadget.** At the time of this commit, these properties have no corresponding `GADGET_PROPERTY` declaration in `gpuRadialSensePrepGadget`. Gadgetron silently ignores unknown XML properties — the gadget will start without error, but the ARKS spoke buffer will not be active. The XML is effectively **forward-declaring** the interface before the implementation exists. This is intentional — the next commit adds the implementation — but running this XML against the current build would not produce ARKS behavior.

**3. Still hardcoded to GPU device 1 and 3 slices.** Inherited from the previous XML, not fixed here.

---

### What this commit represents architecturally

This is the **design document commit** for ARKS. The XML expresses the intended interface — a 5000-TR history buffer with up to 256 spokes per frame — before the C++ exists. The next three commits (`7e09f298`, `2fe06019`, `17f5a127`) are the implementation that makes these properties real.