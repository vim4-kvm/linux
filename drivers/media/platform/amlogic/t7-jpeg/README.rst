T7 HCODEC JPEG encoder
=====================

The ``amlogic,t7-jpeg-encoder`` platform device exposes ``/dev/jpegenc`` for
linear NV12/NV21 to baseline JPEG 4:2:0. It uses the ``vim4jpegenc`` ioctl ABI:
``_IOW('J', n, u32)`` for buffer info (0), firmware initialization (1), a
30-u32 frame command (2), completion stage (3), header/payload lengths (4),
DMA-buf fd import (7), and import release (8). Other commands are rejected.
The command accepts bounded dimensions, strides and allocation extent only;
physical addresses and canvas identifiers are never accepted from userspace.

The mmap layout includes a diagnostic input area, one header page and a 16 MiB
bitstream ring. The live path imports one contiguous DMA-buf, avoids raw pixel
copies and releases its mapping after each completed frame. System-memory
diagnostics copy into the input area. Only one file owns the engine at a time.
Mapped VMAs retain the file and its DMA allocation until the last mapping closes.
Completion ends the firmware's reads before input-engine reset and unmapping.
The mapping uses cached pages with explicit CPU/device ownership. After command
submission, userspace must not touch input or payload until completion and the
lengths ioctl have returned. Between frames only the returned payload extent
may be read; the unused bitstream ring remains device-owned. The header page is
CPU-only. Diagnostic input is synchronized separately from imported DMA-bufs.
Close aborts with a secure power-off before freeing DMA; a failed power-off
quarantines the context and pins the module until reboot.

T7 requires its boot-stage trusted OS, OP-TEE device, boot-completion handshake,
firmware supplicant and licensed video-firmware preloader. The encoder asks the
secure loader for JPEG firmware index 21 on HCODEC core 3. The encrypted firmware
package is provisioned separately; no firmware bytes are included here. The
T7 OP-TEE compatible checks the firmware's runtime dynamic-SHM setting before
using normal RAM for call arguments. A capability bit alone is insufficient.
The assigned HCODEC clock must select the P0 branch at 666666666 Hz. With P1
selected, the trusted loader faults during initialization on this board.

``python3 test_jpeg.py`` compiles actual command/codec functions with ASan/UBSan
and checks allocation boundaries, busy/unpowered states and all quality levels.
Pillow decodes a generated header with a neutral MCU. These offline checks do
not establish hardware operation, cadence, image integrity or recovery; those
require exclusive board testing with a moving source and repeated teardown.
``python3 test_import.py`` checks producer-fence waits, contiguous address
bounds, import failure cleanup and DMA-buf reservation-lock ownership.
