VIM4 external kbase r6: DMA frequency diagnosis
=============================================

The opt-in trial DTS adds a separate 95 C passive GPU cooling map (states
0..4), preserving the existing MCU map. Default Panfrost/5.15 boot files
remain unchanged. The kernel is still f71b5c6289cb, release
7.3.0-vim4kvm-uhd3+, build ID a3572361d542e5505bd159ad8a327cf364fe25f3.

Board locks were acquired after HDR released them, following about 60
minutes of waiting. The window ran 2026-10-31 06:29:40--06:55:18 UTC.
Four independent RAM-selected trials covered the base, performance
governor, dma-coherent restoration and omitted T7 override variants.

The DMA difference was frequency history, not per-submit full-buffer cache
maintenance. With the same contiguous WC heap-gfx contract, vendor 5.15
measured 1.574 ms at 800 MHz, while the 7.3 simple_ondemand trial remained
at 250 MHz and took 3.570 ms. Vendor 5.15 later also dropped to 250 MHz and
took 3.797 ms on codecmm. Selecting the upstream performance governor at
800 MHz gave 1.522 ms; the rebuilt policy module after reboot gave
1.493 ms. Its three independent acceptance runs averaged 1.509 ms versus
1.770 ms for the vendor's three runs. GPU timestamps accounted for most
of the slow 250 MHz wait. No voltage or PM gating changes were made.

The vim4-only default governor is maintained in the external patch series,
0008-t7-devfreq-policy.patch. Other platforms retain simple_ondemand.
Thermal frequency constraints still apply; power/energy parity was not
measured. Per-submit DMA/architecture trace events showed no 24.9 MB
maintenance in the timed intervals on either kernel. Initial/final CPU
access was outside timing. The ION workaround was not removed.

The final board-tested performance artifacts have SHA256 values:

* mali_kbase.ko:
  81cbf9d2354f6b168c0072b56ff85df3e2fab267bbfd3419083345824f1343dd
* 64-bit-mask WC exporter:
  647ae98519a7ad8714ecb703f588b450f9ae0b05e9bdfc21fd452ada00b1de0b
* complete retained-profile candidate DTB:
  63e5b3c77f2370e41b535ad451770db3ace80f169275b952764ce8f6fea3b1c5

Normal candidates had zero kernel WARN/BUG/oops and zero GPU/MMU faults.
Vulkan covered six memory paths including the full CMA profile; CL covered
five unique paths (gfx was duplicated by the runner, so default-CMA CL
was not rerun in this window). EGL/GLES 1.5/3.2, Vulkan device 1.3.231 and
CL device/C 3.0 remained available. The policy build measured GLES 648.903
FPS, CL latency 48.00 us and SP 101.76 GFLOPS, versus vendor 614.091 FPS,
79.66 us and 101.73 GFLOPS. This is not CTS acceptance.

Five devfreq levels passed. GPU cooling was observed bound to trip0 at
95000/passive, current state0/max4; no heat/emulated-temperature test was
performed. HDMI capture plus Wave5 H.264 at 4K60 remained 59.9983 FPS with
GPU work, with 600 measured frames. Actual display-output concurrency was
not established: the blob did not establish a linear scanout BO layout.
NV12 BO export succeeded through the plane-FD API but its modifier was
INVALID, so Wave5 QBUF/encoding was refused rather than guessing layout.

Independent fault isolation retained override while restoring dma-coherent:
all three minimum native/host/gfx loads passed. Omission of override read
back zero and failed the first native device-local load with AS0 translation
fault 0x6c002c1, READ/source0x6c0; region unmapped/free. The case stopped at
the first fault and the board returned to 5.15. Thus override is directly
justified; deleting dma-coherent remains conservative vendor-cache-contract
parity, not a proven necessary fix for those minimum loads. The forced
fault cleanup also exposed a deprecated system_wq message, still pending.

All six protected boot-file hashes and five runtime helpers were restored.
One-KVM was explicitly started after rollback; all four services were
active and UDC configured before releasing both locks. No HID, audio tests,
video ptrace, PWM/voltage writes, blob push or bootloader flashing occurred.

Full results, source-theme/rebase notes and remaining interop gates are in
the vim4-kvm repository at docs/bringup/2026-10-31-kbase-r6-window.md and
docs/gpu-kbase-7x.md. Product promotion/integration approval is not claimed.
