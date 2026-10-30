USB-Blaster II FX2 firmware

File: blaster_6810.hex
Vendor: Intel/Altera
Origin: Quartus Prime Programmer 22.3.0 Build 104, linux64 distribution
File-header version: 1.42
Size: 20760 bytes
SHA-256: 9b32a2162ce47bd18ef19cd0f98eea07aaa52ce8ca2f70939b29c37e5badbeaf

The original file bytes, header and Intel HEX records are preserved.
This firmware runs on the USB-Blaster II probe; it is independent of the
FPGA design and is needed for RBF programming, without SOF conversion.

The vendor firmware is separate from the Apache-2.0 licensed loader source.
This provenance notice does not grant an additional license to the firmware.

CMake installs this file and notice with the CLI when USB-Blaster II support
is enabled. The stratix10gx preset finds the installed firmware automatically;
--probe-firmware and the per-user firmware location remain overrides.
