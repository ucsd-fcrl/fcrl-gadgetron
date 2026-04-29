import importlib, subprocess, sys, os, re

required = {
    "numpy": "1.26.4",
    "h5py": "3.10.0",
    "nibabel": "5.3.2",
    "opencv-python-headless": "4.10.0.84"
}

for pkg, ver in required.items():
    try:
        importlib.import_module(pkg)
    except ImportError:
        subprocess.check_call([sys.executable, "-m", "pip", "install", f"{pkg}=={ver}", "--user"])

# --- Main imports (after ensuring availability) ---
import h5py
import numpy as np
import nibabel as nib

# --- Usage and argument handling ---
if len(sys.argv) != 2:
    print("Usage: python3 h5_to_nii_continuous.py <input_file.h5>")
    sys.exit(1)

input_path = sys.argv[1]

if not os.path.exists(input_path):
    print(f"Error: input file not found: {input_path}")
    sys.exit(1)

# Automatically derive output filename
base_name = os.path.splitext(input_path)[0]
output_path = base_name + ".nii"
print(f"Converting {input_path} → {output_path}")

# --- Load and process data ---
with h5py.File(input_path, "r") as f:
    top_key = list(f.keys())[0]
    print(f"Found top-level group: {top_key}")

    # 🔥 FIX: numeric sort instead of lexicographic sort
    keys = [k for k in f[top_key].keys() if k.startswith("image_")]

    def image_index(name):
        match = re.search(r'image_(\d+)$', name)
        return int(match.group(1)) if match else 10**9

    image_groups = sorted(keys, key=image_index)
    print(f"Found image groups: {image_groups}")

    all_sequences = []
    for grp in image_groups:
        data = f[top_key][grp]["data"][()]
        data = np.squeeze(data)
        print(f"Loaded {grp} with shape {data.shape}")
        all_sequences.append(data)

# --- Concatenate all time sequences ---
continuous = np.concatenate(all_sequences, axis=0)
print("Final concatenated shape:", continuous.shape)

# --- Normalize intensity ---
continuous = continuous.astype(np.float32)
continuous -= continuous.min()
if continuous.max() > 0:
    continuous /= continuous.max()

# --- Save as NIfTI ---
nii_img = nib.Nifti1Image(continuous, affine=np.eye(4))
nib.save(nii_img, output_path)
print(f"✅ Done! Saved continuous sequence as {output_path}")
