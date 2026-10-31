VIM4 external kbase r11 default-policy validation
===============================================

The tested kernel source remains formal 8bca939d6bbd3, uhd5 GNU build ID
90cc3dc2ddd1ad2562bf07e8de65f96eeb092a90. Only external modules were rebuilt.
The main repository's governor commit 1f8c2d9 follows the actual 5.15 live
/bifrost DT: missing def_clk/max_clk means the vendor defaults to level 4,
not the def_clk=0 from a different source DT. High load can jump directly
to 800 MHz. Software PP counts also delay downscaling, despite the vendor
hardware PP setter being a no-op. Both were missing in the r10 policy.
Periodic polling now uses a non-deferrable timer, like the vendor's
periodic wakeup semantics, without claiming identical timer phase.

The real governor decision function was compared against the GPL vendor
state machine for 24,240 frequency/load/hold/software-PP combinations.
The default remains vim4, 100 ms, with thermal QoS; no performance or
fixed-frequency product policy, voltage or hardware core gating was added.

Seven samples after the same five-minute idle history and two-second
probe intervals gave median/P95 values (cached 5.15 in parentheses):

* DMA: 1.535/3.570 ms (1.692/3.618).
* GLES: 646.280/648.065 FPS (616.361/617.773).
* CL latency: 53.19/56.54 us (78.32/79.14).
* CL SP: 101.74/101.76 GFLOPS (101.69/101.71).

The default noncoherent DT passed EGL 1.5/GLES 3.2 pixels, Vulkan 1.3.231,
OpenCL device/C 3.0 and six native/import paths in both compute probes.
Completed default-path tests had zero WARN/BUG/oops/GPU faults. The
five-minute idle residency remained 250 MHz. mpgpu reported about 90/45
percent at continuous/half duty and settled to zero at idle. GPU thermal
emulation changed cooling state 0 to 1 and 800 to 666.7 MHz, then restored
state 0 and 800 MHz after emul_temp was cleared. Five OPPs passed.

The native 4K60 Wave5 benchmark measured 59.998078 FPS with GPU work.
The actual One-KVM service received continuous GLES load while a video-only
browser decoded 2106 frames at 3840x2160 in 35 seconds. Its steady encoder
samples remained around 60 FPS. No HID, sound request or ptrace was used.

The independent negative DT restored only dma-coherent. Six minimal Vulkan
paths passed, but the first native CL kernel produced job slot 1 exception
0x51, INSTR_INVALID_ENC, and CL error -14. It was stopped immediately and
not performance-tested. This is not an AS0 page fault; it rejects that DT
setting without claiming the cache failure's exact microarchitecture.
Keep dma-coherent absent in the tested default DT.

After rollback to 5.15, carrier was up but DHCP failed. Serial recovery
restored helpers and performed one further normal 5.15 reboot; SSH/DHCP
then recovered. Final boot/registration hashes, kernel build ID, four
services, UDC and helper restoration passed. Only volatile reboot_mode
changed from watchdog_reboot to normal. This remains an unattended-rollback
limitation, not a reason to weaken the generic tool's identity checks.

The main tool's compact cleanup command removed the owned empty module
mountpoint, including the r10 marker. 140 evidence files were hash-checked;
only the private GStreamer registry cache was omitted. All board tickets
were released. Formal pins/default boot were not changed and no proprietary
blob was published. See vim4-kvm/docs/bringup/2026-10-31-kbase-r11-policy.md
and docs/gpu-kbase-uhd5-integration.md for the proposal awaiting user review.
