VIM4 external kbase r5: bounded board acceptance
==============================================

The candidate remains opt-in. Default Panfrost and 5.15 boot files were not
modified. The kernel baseline is f71b5c6289cb, release
7.3.0-vim4kvm-uhd3+, build ID a3572361d542e5505bd159ad8a327cf364fe25f3.

The external r44p0 UK 11.39 series now restores T7 power override setup
after GPU reset and the vendor DMA API coherency contract. The vendor GPU
uses system-coherency=0 without dma-coherent. The candidate previously
inherited Panfrost's dma-coherent. Both select DDK ACE-Lite but differ in
DMA API cache maintenance. The opt-in DTS now matches the vendor choice.
No SG/GUP/MAIR encoding or voltage changes were needed in this window.

A separate GPL external module implements WC system-uncached pages and
WC contiguous heap-gfx allocations from a dedicated reusable CMA pool.
The candidate adds a 128 MiB pool, compared with the vendor's 300 MiB.
This is not a cached system/CMA alias. The exporter rejects fallback pages
outside the dedicated pool and cannot unload until reboot because the
baseline public heap API provides no unregister operation.

The sources, build recipe, host mmap/pool boundary tests and complete
patch series live in the vim4-kvm repository under packaging/kbase.
Both modules pass W=1, KCFLAGS=-Werror, modpost/BTF and unchanged UK ABI.
Their board-tested SHA256 values are:

* mali_kbase.ko:
  19f41217a32db1f8e44e80146616473a3cfca980e355c9cb4543014bc94c488a
* vim4_kbase_heaps.ko:
  f4b12d22f600137e4f81f97f9cdbb460980ffa0f8f390afa98786a4fd9ea3936
* candidate DTB, derived from the complete retained display profile:
  a2ce787f9064983b7c558c1b2820bf58b4c2952b2c442fd33ad8e2afe6dee3ea

Board window: 2026-10-31 04:59:57--05:11:28 UTC (October 30 task date).
Waited for HDR to release board/heavy locks before connecting. A static
private init checked the build ID and mounted the complete matching module
set. RAM sysboot selected the private candidate; no default was changed.

There were zero kernel WARN/BUG/oops and zero GPU/MMU faults or DEVICE_LOST.
Native device-local/host-visible, system/CMA and both new WC heaps passed
independent 16 KiB Vulkan/OpenCL kernels with full readback verification.
On this UMA device the native Vulkan types overlap (type 0, flags 0x7).
EGL 1.5, GLES 3.2, Vulkan 1.3.231 and CL device/C 3.0 matched the vendor;
extension sets were identical. GBM BO export and a separate display DRM
file's PRIME import passed with render and scanout usage. Additional LINEAR
usage failed on both stacks. Actual scanout and Wave5 import of that BO
were not tested.

Paired means: GLES 619.825 -> 648.419 FPS; 25 MB Vulkan DMA 1.708 ->
3.571 ms; CL latency 80.30 -> 51.05 us; CL SP 101.73 -> 101.75 GFLOPS.
DMA exporters differed (vendor codecmm versus cached default CMA). A large
WC heap-gfx comparison is still needed; this is not full performance parity.
The combined reset/coherency repair was not split into single-change A/B,
so it does not prove which change alone fixes the original shareability
and translation faults. Old fault VAs cannot be assigned to a lost region
map retrospectively.

Five devfreq levels passed loads and frequency readback. GPU cooling
registered with max_state=4 but has no GPU thermal-zone binding: the
retained 95 C map targets the MCU, not the GPU. This acceptance item failed
and must be fixed/tested in an explicit future candidate DT. No on-board DT
or driver repair was attempted. HDMI/Wave5 4K60 alone/with GPU both sustained
59.9983 FPS for 600 measured frames; no bitstream quality test was made.

The board returned to 5.15.137-vim4kvm-hdrdual6. All four services were
active, UDC configured, six boot-file hashes unchanged, and five runtime
helpers restored. Locks were released immediately after recovery/evidence
checks. Full results and limits: vim4-kvm repository,
docs/bringup/2026-10-30-kbase-r5-window.md. No blob is included.

Final offline exporter correction, not board-tested
-------------------------------------------------

After releasing the window, review found that the exporter's 32-bit DMA
mask could bounce high-address system pages during initial cache
maintenance. The final source now matches the vendor's 64-bit heap device
mask. The separate gfx CMA pool/range checks still constrain gfx pages
below 4 GiB. Small-buffer success does not establish correctness of the
old mask under cache eviction or high-memory pressure.

The corrected exporter passes Werror/modpost, host tests and strict style;
its SHA256 is
ac508a4e378155493aad704a6cafae00c6c744188ff14e4f73487226aa96d939.
HDR2_R15 had reacquired both locks, so no second board connection or boot
was attempted. Board results above describe the earlier release3 artifact,
not hardware acceptance of the final release4 exporter. Retest the final
heap and high-address pages before promoting or adding the thermal map.
