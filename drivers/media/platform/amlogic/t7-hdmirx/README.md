# T7 HDMI receiver bring-up

The initial driver owns the receiver TOP MMIO window, HDMIRX power domain and
pclk/CFG/METER/2M/5M clocks. It reads source 5V and the HPD request register through
`/sys/kernel/debug/fe398000.hdmi-receiver/status`. It does not initialize the PHY,
program EDID/HPD, request interrupts or start VDIN DMA. No capture device is exposed.
The normal VIM4 DTB leaves the receiver disabled; use the dedicated hdmirx-test
DTB for bring-up. Its port 1 detect pin is muxed without changing HPD/DDC pins.

Hardware access is permitted only after runtime power and all clocks are held.
Managed cleanup removes debugfs before callback storage and clocks are released,
then drops the power-domain reference. The status file is diagnostic, not a
stable userspace ABI. `source_5v` indicates cable power, not TMDS/PHY lock.
`hpd_request` is a register value, not proof of the physical HPD voltage.

Next stages are EDID/DDC/HPD, T7 PHY tuning and signal lock, then an independent
VDIN capture path using completed frames and explicit DMA buffer ownership.
Existing 5.15 TVIN/VFM/codec_mm interfaces must be replaced or explicitly adapted;
the Synopsys rk3588 capture driver is not a drop-in T7 frontend.
