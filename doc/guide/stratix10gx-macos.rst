Stratix 10 GX RBF programming on macOS
#####################################

This feature loads an existing RBF into SRAM on the tested Intel
DK-DEV-1SGX-L-A kit, TA J99985-001, FPGA 1SG280LU2F50E2VG, using the onboard
USB-Blaster II. Other Stratix 10 variants have not been verified. SDM flash
programming and native SOF conversion are not supported by this branch.

Build and install
=================

Install Apple's Command Line Tools if necessary, then run from this checkout:

.. code-block:: bash

    brew install cmake pkgconf libusb libftdi hidapi zlib python
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
        -DMACOS_BUNDLE_LIBRARIES=ON \
        -DCMAKE_INSTALL_PREFIX="$PWD/build/openFPGALoader-macos"
    cmake --build build --parallel
    cmake --install build
    ./build/openFPGALoader-macos/bin/openFPGALoader --version
    ./build/openFPGALoader-macos/bin/openFPGALoader -b stratix10gx project.rbf

The compiler builds for the host architecture by default. The same commands
apply to Apple Silicon and Intel Macs; the bundled libraries must support the
selected architecture. Apple Silicon macOS is the tested host.

Homebrew supplies build dependencies. The installed folder includes the
linked non-system libraries and USB-Blaster II firmware. Quartus is not
needed for building, installing or programming an existing RBF. Python and
Homebrew are not runtime dependencies of the finished installation.

Keep the entire build/openFPGALoader-macos folder together when moving or
zipping it. It contains bin/openFPGALoader, lib/openFPGALoader and share.
The executable finds its libraries and firmware relative to its location,
including when launched through a PATH symlink. Explicit --probe-firmware
and --index-chain options remain available; existing per-user firmware
configuration takes precedence over the bundled firmware.

MACOS_BUNDLE_LIBRARIES is off by default for ordinary upstream builds.
When enabled, installation copies the linked non-system dylibs, rewrites
their paths, and ad-hoc signs the executable and library copies. License
texts and available Homebrew recipes accompany the libraries under
share/licenses/openFPGALoader. The installation is not Apple-notarized.

Normal builds also copy the probe firmware into build/share, so the macOS
build/openFPGALoader executable can find it before installation. That
uninstalled executable still uses the host's libraries.

Supply an RBF
=============

Use an RBF generated separately with the appropriate Quartus tools for this
device. This branch does not accept a SOF directly or assemble a new RBF.
The USB-Blaster firmware is separate from the FPGA design and is already
included; its provenance and SHA-256 are in firmware/usbBlasterII/README.txt.

Earlier RBF loading and application behavior were confirmed on the kit.
Current hardware retesting and Windows runtime validation remain deferred.

USB-Blaster II timeout handling
==============================

A successful firmware-version read does not guarantee that the first TDO
reply will arrive within one second. TDO reads allow up to three seconds
per reply and retain any bytes delivered with a USB timeout. Only the read
is retried: replaying a JTAG command would clock the TAP again and corrupt
the scan. Disconnects, endpoint stalls and overflows fail immediately;
an incomplete reply also fails instead of continuing with missing data.

Initial chain detection allows ten attempts after a TDO timeout. Each
retry closes and reopens the probe, clears endpoint halts, drains stale
TDO, clears queued software TAP transitions and resets the hardware TAP
before scanning the chain again. A scan that failed midway is never
continued. Disconnects and other transport errors do not trigger this
recovery. Once initialization finishes, a timeout aborts the operation;
active programming is not restarted automatically.

Startup drains both replies already queued on USB and samples still held
inside the probe. After the command-FIFO wipe restores command boundaries,
the loader sends the TDO-copy command and discards its reply before the
first scan. Endpoint-clear failures, drain errors and continuously arriving
stale data abort initialization. Closing the probe always releases its USB
handle, including when the device has disconnected.

FX2 firmware loading rejects parser errors and empty or incomplete images
before resetting the CPU. Control transfers must complete their requested
length, and firmware-loading failures release the claimed interface,
device handle and USB context. Tests also load the bundled HEX through the
mock transport to check the successful upload path.

On Stratix 10 chains, initialization also sends known marker bits through
the instruction register and BYPASS data path and compares their returned
bits. This checks the JTAG scan path before programming, including chains
without the extra scan stage observed on the tested kit. It does not read
back FPGA configuration memory or compare it against the RBF. Programming
completion is checked using SDM configuration status and CONF_DONE.

Each RBF chunk receives an SDM flow-control status check before the next
chunk is sent. Device errors, backward acknowledgments, offsets beyond the
sent data and persistent lack of progress abort the transfer. Only the
unaccepted suffix may be resent when the device accepts part of a chunk.
Busy polling is limited to 6000 polls or 30 seconds per chunk, whichever
comes first. Empty or non-word-aligned RBF input is rejected before SDM
configuration begins. Final CONFIG_STATUS validation remains mandatory
after every byte is acknowledged. These checks are always enabled; they
are device status checks, not byte-for-byte configuration-memory readback.

The transport tests simulate delayed and split replies, startup recovery,
stale TDO in both buffers, retry exhaustion, cleanup failures and corrupted
marker readback without hardware. Timeout injection covers every startup
reply boundary.
An independent SDM model also exercises the production Altera programming
path with partial acceptance, device errors, forward/backward offsets,
stalls, invalid input and missing final CONF_DONE.
Intermittent failures on a physical probe still require hardware retesting.
