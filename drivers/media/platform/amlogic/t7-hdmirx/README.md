# T7 HDMI receiver bring-up

The initial driver owns the receiver TOP MMIO window, HDMIRX power domain and
pclk/sys/CFG/METER/2M/5M clocks. It reads source 5V and the HPD request register through
`/sys/kernel/debug/fe398000.hdmi-receiver/status`. No capture device is exposed;
Automatic PHY tuning, receive interrupts and VDIN DMA still require implementation.
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

An explicit `phy_test=1` parameter adds a bounded input PLL/equalizer trial;
it requires a digital base-block-only EDID with a 148.5 MHz preferred timing,
and an input cable clock near 148.5 MHz. CTA/SCDC EDIDs are rejected: HDMI 2.0's
1:40 clock ratio makes a 594 MHz stream ambiguous at the cable meter. Other rates,
missing 5V, unstable clocks or three failed PLL attempts fail probe and power
down the analog path. The trial owns a separate PHY MMIO resource, preserves
the boot-provided termination trim, and restores its TOP configuration at exit.
It does not retry asynchronously, tune arbitrary cables, or expose captured
timings. A PLL lock bit alone does not establish an aligned TMDS/video stream.
Status reports raw cable/TMDS meters, SQO, alignment and PLL lock separately.
Run `python3 test_phy.py` for clock qualification, bounded failure and cleanup
regressions against the actual C functions with fake MMIO.

Next stages are T7 PHY tuning and signal lock, then an independent
VDIN capture path using completed frames and explicit DMA buffer ownership.
Existing 5.15 TVIN/VFM/codec_mm interfaces must be replaced or explicitly adapted;
the Synopsys rk3588 capture driver is not a drop-in T7 frontend.

## Direct capture experiment and local benchmark

The VDIN V4L2 queue defaults to the private cached ring/copy path. The read-only
module option `direct_dma=1` instead arms coherent vb2 allocations taken from the
queued list. Only a final AXI completion can publish a buffer. A dequeued buffer
is never armed again until the consumer queues it; starvation pauses capture,
and resuming discards a possibly partial first frame. The encoder still holds
the capture-pool lease while importing its DMA planes. No CPU frame copy or
private-ring cache synchronization is performed in direct mode.

Each direct buffer acquires an independent allocator DMA-buf lease in buf_init,
before it can reach an IRQ. STREAMOFF joins the worker and synchronizes DMA/IRQs
before vb2 teardown. On a stop timeout, ERROR contents are invalid, the queue
cannot restart, and allocator leases of hardware-used buffers are intentionally
retained until reboot along with the existing module/private-memory retention.
Thus REQBUFS(0), close, and loss of userspace exports cannot free or reuse memory
that a failed-stop MIF might still write. Suppressed platform bind attributes
and the fault module reference prevent normal driver removal in that state.
Unused allocations are released normally. `stream_copy_bytes` in debugfs counts
CPU frame-copy bytes in the current stream; direct mode must report zero.

`python3 test_stream.py` compiles actual IRQ/queue/worker/cleanup C code with
controlled hardware gates and ASan/UBSan. It covers both paths, held buffers,
starvation/requeue, in-flight STREAMOFF, busy/missing IRQ quarantine, allocator
lease failure, and memory surviving destruction of vb2 metadata. These checks
do not prove hardware timing, DMA import or frame correctness.

On Linux ARM64, `python3 test_copy.py --output /tmp/vdin-copy-bench` builds a
standalone comparison of the actual kernel memcpy_generic and a SIMD candidate.
Run its `--benchmark /dev/videoN` mode only with an idle t7-vdin-test queue. It
allocates four real vb2 MMAP buffers, alternates implementations, validates full
outputs, and never starts capture. Ordinary cached sources omit source DMA
synchronization and kernel SIMD context costs. The SIMD candidate is not linked
into the module; it showed no benefit in the initial VIM4 mapping measurements.
