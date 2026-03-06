# Gadgetron Image Reconstruction Framework

The Gadgetron is an open source project for medical image reconstruction. If you find the Gadgetron useful in your research, please cite this paper:

Hansen MS, Sørensen TS. Gadgetron: An Open Source Framework for Medical Image Reconstruction. Magn Reson Med. 2013 Jun;69(6):1768-76.

Documentation for the project is available at [https://gadgetron.readthedocs.io](https://gadgetron.readthedocs.io)

## License

The Gadgetron is available under a modified MIT license. Please read [LICENSE](LICENSE) file for licensing details.

# FCRL Golden Radial Mode4 Reconstruction Pipeline

This repository documents the workflow for running a custom **Gadgetron reconstruction pipeline** using the configuration:
fcrl_golden_radial_mode4_read_angle_realtime.xml
# Project Overview

This pipeline reconstructs **FCRL golden radial acquisitions** using a custom Gadgetron XML pipeline that reads trajectory angles during reconstruction.

The full workflow includes:

1. Building Gadgetron inside a conda environment
2. Activating the Gadgetron environment
3. Installing the custom XML reconstruction configuration
4. Running dependency / noise measurement
5. Running the main reconstruction

---

# Recommended Project Structure

Example working directory:
gadgetron_source/
│
├── build/
├── gadgets/
├── toolboxes/
├── test/
│
├── fcrl_golden_radial_mode4_read_angle_realtime.xml
├── README.md
└── CMakeLists.txt


The reconstruction XML file is maintained in the repository and copied into the Gadgetron configuration directory.

---

# 1. Activate the Gadgetron Environment 
---
source /opt/conda/etc/profile.d/conda.sh
conda activate gadgetron

# 2. Build Gadgetron (Conda Environment)
Inside the conda environment, build Gadgetron using the following steps.
---
cd gadgetron
mkdir -p build
cd build

cmake -GNinja \
-DCMAKE_BUILD_TYPE=Release \
-DUSE_MKL=ON \
-DCMAKE_INSTALL_PREFIX=${CONDA_PREFIX} \
../

ninja
ninja install

# 3. Install Custom Reconstruction XML
Copy the reconstruction XML file into the Gadgetron configuration directory.
---
cp ~/gadgetron_source/fcrl_golden_radial_mode4_read_angle_realtime.xml \
/opt/conda/envs/gadgetron/share/gadgetron/config/

# 4. Define Reconstruction Configurations
Define the configuration variables.
---
CFG=fcrl_golden_radial_mode4_read_angle_realtime.xml
CFG_noise=default_measurement_dependencies.xml

# 5. Siemens Data Conversion
Siemens raw MRI data is typically stored in .dat files.
Gadgetron uses the ISMRMRD format (.h5) for reconstruction.
The Siemens raw data must therefore be converted before reconstruction.
---
siemens_to_ismrmrd \
-f /work/"file_name".dat \
-o /work/"file_name".h5 \
-Z

Example:
siemens_to_ismrmrd \
-f /work/meas_MID00347.dat \
-o /work/meas_MID00347.h5 \
-Z

# 6. Run Dependency / Noise Measurement
gadgetron_ismrmrd_client \
-a localhost -p "90xx" \
-f /work/"file_name_1".h5 \
-c "$CFG_noise"

Example:
gadgetron_ismrmrd_client \
-a localhost -p 9088 \
-f /work/meas_MID00090_FID08384_fcrl_beat_arks_5k_ARKS_SaxSlc_BH_9spokes_1.h5 \
-c "$CFG_noise"

# 7. Run Main Reconstruction
gadgetron_ismrmrd_client \
-a localhost -p "90xx" \
-f /work/"file_name_2".h5 \
-c "$CFG" \
-o /work/"out_file_name_1".h5

Example:
gadgetron_ismrmrd_client \
-a localhost -p 9088 \
-f /work/meas_MID00090_FID08384_fcrl_beat_arks_5k_ARKS_SaxSlc_BH_9spokes_2.h5 \
-c "$CFG" \
-o /work/fcrl_out_meas_FID08384.h5