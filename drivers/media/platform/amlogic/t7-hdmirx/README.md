# T7 HDMI receiver bring-up

The initial driver owns the receiver TOP MMIO window, HDMIRX power domain and
pclk/sys/CFG/METER/2M/5M clocks. It reads source 5V and the HPD request register through
`/sys/kernel/debug/fe398000.hdmi-receiver/status`. No capture device is exposed;
PHY tuning, receive interrupts and VDIN DMA still require implementation.
The normal VIM4 DTB leaves the receiver disabled; use the dedicated hdmirx-test
DTB for bring-up. Its port 1 detect, HPD and DDC pins are muxed for the receiver.

With no parameters, the driver only monitors resources. To publish a dedicated
test EDID, install it under `/lib/firmware` and load the module with
`edid_firmware=directory/file.bin`. Only complete, checksummed 128..512 byte EDIDs
whose extension count matches the size are accepted. The selected port's RAM is
written and read back before HPD is asserted. Unload drops HPD and restores TOP
reset/port state before releasing clocks and power. The read-only debugfs `edid`
file contains the verified published bytes; `status` also reports the DDC state.
There is no live EDID replacement API at this stage; changing a test EDID requires
unloading and reloading the module, which disconnects the source.

Hardware access is permitted only after runtime power and all clocks are held.
Managed cleanup removes debugfs before callback storage and clocks are released,
then drops the power-domain reference. The status file is diagnostic, not a
stable userspace ABI. `source_5v` indicates cable power, not TMDS/PHY lock.
`hpd_request` is a register value, not proof of the physical HPD voltage.

Run `python3 test_ddc.py` for the actual validation/publication code with fake MMIO.
It checks malformed inputs, HPD ordering, port-bank isolation and readback failure.
Source-side hardware validation must compare at least two distinct EDIDs to avoid
treating cached display metadata as a new DDC read. It does not validate every
advertised display mode or establish physical HPD voltage/timing.

Next stages are T7 PHY tuning and signal lock, then an independent
VDIN capture path using completed frames and explicit DMA buffer ownership.
Existing 5.15 TVIN/VFM/codec_mm interfaces must be replaced or explicitly adapted;
the Synopsys rk3588 capture driver is not a drop-in T7 frontend.
