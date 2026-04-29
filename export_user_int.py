#!/usr/bin/env python3
"""
Dump the user_int[8] and user_float[8] arrays from an ISMRMRD .h5 file as-is.
No ICE param remapping -- just the raw header field names.

This reflects exactly what the (unpatched) siemens_to_ismrmrd converter writes:
    user_int[0..6]   <- Siemens IceProgramPara[0..6]
    user_int[7]      <- ulTimeSinceLastRF   (NOT ICE param 7)
    user_float[0..7] <- Siemens IceProgramPara[8..15]

Output CSV columns:
    acq_index, scan_counter, k1, slice, phase, repetition,
    user_int_0..user_int_7,
    user_float_0..user_float_7

Usage:
    python read_ice_params.py data.h5
    python read_ice_params.py data.h5 out.csv
"""

import sys
import csv
import ismrmrd
from pathlib import Path


def read_ice_params(h5_filename, output_csv=None):
    if output_csv is None:
        output_csv = Path(h5_filename).stem + "_ice_params.csv"

    print(f"Reading: {h5_filename}")
    print(f"Output:  {output_csv}")

    dataset = ismrmrd.Dataset(h5_filename, 'dataset', create_if_needed=False)
    n_acq = dataset.number_of_acquisitions()
    print(f"Acquisitions: {n_acq}")

    with open(output_csv, 'w', newline='') as f:
        w = csv.writer(f)

        # Header: metadata + raw user_int[8] + raw user_float[8]
        header = ['acq_index', 'scan_counter', 'k1', 'slice', 'phase', 'repetition']
        header += [f'user_int_{i}'   for i in range(8)]   # all 8 slots, including [7]
        header += [f'user_float_{i}' for i in range(8)]
        w.writerow(header)

        for i in range(n_acq):
            acq = dataset.read_acquisition(i)

            row = [
                i,
                acq.scan_counter,
                acq.idx.kspace_encode_step_1,
                acq.idx.slice,
                acq.idx.phase,
                acq.idx.repetition,
            ]
            # Raw user_int[0..7]  -- 8 int32 values exactly as in the .h5
            row += [int(acq.user_int[j]) for j in range(8)]
            # Raw user_float[0..7] -- 8 float32 values exactly as in the .h5
            row += [float(acq.user_float[j]) for j in range(8)]

            w.writerow(row)

    dataset.close()
    print(f"Done: {n_acq} rows x {len(header)} columns")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python read_ice_params.py <file.h5> [out.csv]")
        sys.exit(1)
    read_ice_params(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)