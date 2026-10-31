VIM4 external kbase uhd5 trial
=============================

The candidate DTS inherits the complete formal uhd4 display-m0 profile at
8bca939d6bbd3e4d91e9f23b812e920172c99b3d. It changes only GPU ownership,
the separate WC heap CMA pool and the GPU frequency cooling map. The
Panfrost DT and the default 5.15 boot selection remain unchanged.

The uhd5 build changes LOCALVERSION and enables THERMAL_EMULATION. The
release is 7.3.0-vim4kvm-uhd5+, GNU build ID
90cc3dc2ddd1ad2562bf07e8de65f96eeb092a90. The configuration patch and all
external DDK/governor/heaps sources live in vim4-kvm/packaging/kbase, not
in this kernel tree. Later external-module fixes reused this Image and
its matching BTF/Module.symvers; no formal kernel output was modified.

The default governor is the separate vim4 module, with the vendor-derived
100 ms period, five thresholds and low-load hold counters. Performance is
an explicit diagnostic choice, never the product default. Devfreq retains
control of OPP and thermal QoS constraints. The first automatic async probe
exposed a synchronous governor request WARN and blocked other module
loading. Theme 0008 now supplies a pre: governor_vim4 soft dependency;
the build checks actual module metadata and staged modprobe ordering.

The repaired 2026-10-31 RAM trial passed EGL 1.5/GLES 3.2 pixels, Vulkan
1.3.231 and OpenCL device/C 3.0. Six native/import memory paths passed
both compute probes, including default CMA. All completed tests on this
repaired candidate had zero WARN/BUG/oops/GPU faults. This is not CTS.

Seven samples used the same WC heap-gfx, five-minute idle history and
probe binaries as the cached 5.15 baseline. Median/P95 results were:

* DMA: 1.897/3.557 ms versus 1.692/3.618 ms on 5.15.
* GLES: 646.122/649.165 FPS versus 616.361/617.773 FPS.
* CL latency: 51.60/53.61 us versus 78.32/79.14 us.
* CL SP: 99.77/101.00 GFLOPS versus 101.69/101.71 GFLOPS.

DMA median and CL SP fail parity. The performance gate remains closed;
frequency at probe exit does not prove residency throughout each kernel.
No performance-governor numbers replace these default-policy samples.

mpgpu utilization averaged 90.24 percent under continuous load and 47.60
percent at half duty; idle settled to zero. Five frequency levels passed.
GPU-only emul_temp=96000 changed cooling state 0 to 1 and frequency from
800 to 666.7 MHz. Clearing emul_temp restored state 0 and 800 MHz. No
trip, PWM or regulator setting was changed.

The native 4K60 Wave5 H.264 benchmark measured 59.993619 FPS alone and
59.997769 FPS with GPU submissions (600 frames each). One-KVM service
concurrency did not finish: the test queried HTTP before the restarted
service was ready. The runner then rolled back, so the independent
dma-coherent DT was not booted. Neither gate is claimed as passed.

Both RAM trials returned to default 5.15; protected boot/registration
hashes, four services, UDC and temporary helper restoration were checked.
The generic tool's final empty-module-directory cleanup failed because
it passed the full inventory in a single SSH exec argument; the separate
tool branch fixes that and still awaits hardware retest. Retained private
trial directories and ownership markers must not be recursively removed.

Full evidence, artifact hashes and remaining gates are documented in
vim4-kvm/docs/bringup/2026-10-31-kbase-r10-uhd5.md. Product promotion,
formal source-lock changes and proprietary binary distribution are not
authorized by these partial results.
