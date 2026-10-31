VIM4 external kbase candidate: first board gate
=============================================

This branch's DT is an opt-in trial, not the default Panfrost DT.
The first trial on 2026-10-30 (2026-10-31 UTC) failed the loading gate.
No driver or DT changes were made on the board after the failure.

The kernel was f71b5c6289cb, build ID
a3572361d542e5505bd159ad8a327cf364fe25f3. The external r44p0 UK 11.39
module SHA256 was
f41e1573dba86b5d5054190629d69ef5c451818a8097b2f1ceec0d0bfefdc4b8.
The complete matching module tree was mounted by a private static init.

The GPU was identified and /dev/mali0 was created, but initialization
triggered new warnings in __alloc_workqueue (mali_mmu0 lacked an explicit
WQ_PERCPU/WQ_UNBOUND choice) and __hrtimer_setup (a NULL callback).
The old hrtimer_init compatibility wrapper does not satisfy the new
callback-at-setup contract. The job-fault workqueue name was also truncated.
Uppercase IRQ lookups logged ENXIO; later lowercase lookups/probe succeeded.

The installed rootfs runtime helper rejected the candidate kernel, leaving
HDMI/gadget services unavailable. This is a separate integration blocker.
No userspace GPU capability or performance tests ran on the candidate.

The board was rebooted normally to 5.15.137-vim4kvm-hdrdual6. One-KVM,
HDMI, gadget and manager services returned active, UDC was configured,
and all six protected boot-file hashes matched their entry values.

Full evidence and the required offline follow-up are recorded in the
vim4-kvm repository, branch gpu-kbase-7x, at
docs/bringup/2026-10-30-kbase-window.md. No blob is included here.

Offline r3 follow-up (not yet board-tested)
-----------------------------------------

The external series now appends 0005-runtime-api.patch. It provides callback
at hrtimer setup time, explicitly preserves bound queue semantics with
WQ_PERCPU, and shortens the fault queue name. The reset stack timer is
cancelled after both reset waits before it can be destroyed or restarted.
The pass also fixes OPP node/table cleanup, IRQ probe deferral and the old
duplicate-fence-signal error contract through the public fence lock API.

The host semantic check compiles the actual wrappers against legacy/current
API models and rejects raw timer/workqueue calls in the resulting sources.
The G52 JM module passes W=1, KCFLAGS=-Werror and modpost with unchanged
UK 11.39. Its delivery SHA256 is
4ff4bbf3490d34a3f5f59e3b8b92f33c6199f6ae0d7dcf533ec1f1f26163359d.
No DT or in-tree driver behavior changed in this follow-up. Rootfs runtime
snapshots and the revised bounded CL probes live in the vim4-kvm repository.
See docs/gpu-kbase-repair.md there for audit limits and the next board gate.
