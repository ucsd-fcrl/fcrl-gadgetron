
---

## Commit `2c846c5e` — Add user_int option to select the source of customized angles
**Date:** Apr 1, 2026 (~3.5 weeks after the previous commit)

A focused, clean commit. One problem was discovered between commits: the previous code hardcoded `user_int[1]` as the angle slot, but the actual pulse sequence was storing angles in `user_int[2]`. This commit makes the slot index configurable rather than patching in another hardcoded value.

---

### What changed

#### New XML property: `fcrl_custom_angle_user_int_index`

Added to both `.h` files with a **default of 1**:
```xml
<property><name>fcrl_custom_angle_user_int_index</name><value>2</value></property>
```

The XML config was updated to pass `2`, overriding the default. This tells the gadget to read angles from `user_int[2]` instead of `user_int[1]`.

#### Bounds validation added at `process_config()`:
```cpp
if (fcrl_custom_angle_user_int_index_ < 0 || fcrl_custom_angle_user_int_index_ >= ISMRMRD::ISMRMRD_USER_INTS) {
    GDEBUG("FCRL: Invalid user_int index %d ... Using default 1.\n", ...);
    fcrl_custom_angle_user_int_index_ = 1;
}
```

`ISMRMRD::ISMRMRD_USER_INTS` is the compile-time constant defining the number of `user_int` slots in the ISMRMRD acquisition header (8 in the standard). Any out-of-range value is caught here and falls back to index 1 with a log message — this prevents an out-of-bounds array access at runtime.

#### All three `user_int[1]` hardcoded accesses replaced:
- The angle read in `process()`
- The debug log line
- The `GDEBUG` at config time

All now use `fcrl_custom_angle_user_int_index_`.

---

### Why `user_int[2]` and not `user_int[1]`?

The ISMRMRD `user_int` array is an 8-element scratch space the pulse sequence can write arbitrary integers into. The commit history tells the story:

- **Commit `494ba5c2`** assumed `user_int[1]` was the angle slot — based on the note in the message: *"customized angle reading need to be saved in user_int_1"*
- **This commit** reveals the actual pulse sequence (`meas_FID08384`) stored angles in `user_int[2]` (1-indexed as `user_int_2` in Siemens convention, 0-indexed as `[2]` in C++)

The default in code remains `1` but the XML overrides it to `2`. This means if someone runs the pipeline **without** the `fcrl_custom_angle_user_int_index` property in their XML, they will silently read from the wrong slot and get garbage angles with no error.

---

### Issues to flag

**1. Default value mismatch between code and XML.** The property defaults to `1` in the C++ code, but the XML explicitly sets it to `2`. Any XML config that omits this property will read from `user_int[1]` — almost certainly the wrong slot. The default should be `2` to match the actual pulse sequence, or the documentation should make this prominent.

**2. Bounds validation is `GDEBUG` only.** If an invalid index is passed, the fallback to `1` is logged only at debug level. In production Gadgetron without debug logging enabled, this misconfiguration is invisible. A `GWARN` or `GERROR` would be more appropriate.

**3. The `×10000` fixed-point encoding is still undocumented and unvalidated.** After decoding, there is no check that the result is a plausible angle (e.g., that `user_int[2] != 0` before the first spoke, or that the value is in a reasonable range). A zero in `user_int[2]` would decode silently to `angle = 0.0` rad for every spoke.

**4. Duplicate code still not consolidated.** The FCRL `.cpp` file received identical changes — still two copies of the same logic.

---

### Summary table

| Change | Effect | Risk |
|--------|--------|------|
| `fcrl_custom_angle_user_int_index` property | Slot is now configurable | Low — clean improvement |
| Default value = 1 in code, 2 in XML | Mismatched default | **Medium** — wrong slot if property omitted from XML |
| Bounds check at config time | Prevents out-of-bounds array access | Low — good defensive code, but log level too quiet |
| XML updated to `value=2` | Matches actual pulse sequence | Low — correct for `meas_FID08384` |