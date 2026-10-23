#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual FIO/reset C with a scripted MMIO device and virtual time.

This verifies host ordering and error propagation, not hardware DMA completion,
firmware context preservation, or recovery of an M2M job.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
hardware = (root / 'wave5-hw.c').read_text()
polling = hardware[hardware.index('static int wave5_fio_readl('):
                   hardware.index('bool wave5_vpu_is_init(')]
reset = hardware[hardware.index('int wave5_vpu_reset('):
                 hardware.index('int wave5_vpu_dec_finish_seq(')]
command_check = hardware[hardware.index('static int wave5_vpu_firmware_command_queue_error_check('):
                         hardware.index('static int send_firmware_command(')]
sleep_wake = hardware[hardware.index('int wave5_vpu_sleep_wake('):
                      hardware.index('int wave5_vpu_reset(')]
defines = '\n'.join(line for line in hardware.splitlines()
                    if line.startswith(('#define FIO_', '#define VPU_BUSY_CHECK_TIMEOUT',
                                        '#define FASTIO_ADDRESS_MASK',
                                        '#define W521_FEATURE_BACKBONE',
                                        '#define W521_FEATURE_VCORE_BACKBONE',
                                        '#define W521_FEATURE_VCPU_BACKBONE')))
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int64_t ktime_t;
typedef uintptr_t dma_addr_t;
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define FIELD_GET(mask, value) (((value) & (mask)) / ((mask) & -(mask)))
#define dev_dbg(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_dbg_ratelimited(...) ((void)0)
#define PRINT_REG_ERR(...) ((void)0)
#define barrier() ((void)0)
#define cpu_relax() ((void)0)
#define might_sleep_if(x) ((void)(x))
#define WAVE5_SYSERR_VPU_STILL_RUNNING 0x1000
enum sw_reset_mode { SW_RESET_SAFETY, SW_RESET_FORCE, SW_RESET_ON_BOOT };
struct vpu_buf { dma_addr_t daddr; size_t size; };
struct vpu_attr {
    bool support_backbone, support_vcore_backbone, support_vcpu_backbone;
};
struct vpu_device {
    struct vpu_attr attr;
    u32 product_code, bus_idle_value, reset_mask;
    bool legacy_firmware;
    struct vpu_buf common_mem, sram_buf;
};
struct device { struct vpu_device *data; };
static struct {
    ktime_t now;
    u32 config, indirect[0x10000 / 4], address, data;
    bool fio_write, idle_seen, never_idle, reset_stuck, reset_done;
    unsigned fio_starts, fail_fio_at, fio_data_reads, busy_reads;
    unsigned reset_requests, reenables, writes, sleeps, wakes;
    unsigned enable_first, command;
    unsigned ready_delay, ctrl_reads;
    int sleep_ret, wake_ret;
} hw;
static ktime_t ktime_get(void) { hw.now += 1000; return hw.now; }
static ktime_t ktime_add_us(ktime_t t, u64 us) { return t + us; }
static s64 ktime_us_delta(ktime_t a, ktime_t b) { return a - b; }
static int ktime_compare(ktime_t a, ktime_t b) { return (a > b) - (a < b); }
static bool ktime_before(ktime_t a, ktime_t b) { return a < b; }
static void usleep_range(unsigned a, unsigned b) { (void)b; hw.now += a; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
/* Runtime isolation is disabled during initial probe/reset. */
static bool wave5_vpu_isolated(struct vpu_device *dev) { (void)dev; return false; }
static bool wave5_vpu_inject_timeout(struct vpu_device *dev, u32 command)
{ (void)dev; (void)command; return false; }
static void wave5_vpu_isolate(struct vpu_device *dev, const char *reason, u32 command)
{ (void)dev; (void)reason; (void)command; }
'''
mmio = r'''
static bool is_bus_control(u32 addr)
{
    return addr == W5_GDI_BUS_CTRL || addr == W5_COMBINED_BACKBONE_BUS_CTRL ||
           addr == W5_BACKBONE_BUS_CTRL_VCORE0 || addr == W5_BACKBONE_BUS_CTRL_VCPU;
}
static void wave5_vdi_write_register(struct vpu_device *dev, u32 addr, u32 value)
{
    (void)dev;
    hw.writes++;
    if (addr == W5_VPU_FIO_DATA) hw.data = value;
    if (addr == W5_VPU_FIO_CTRL_ADDR) {
        hw.fio_starts++;
        hw.ctrl_reads = 0;
        hw.address = value & 0xffff;
        hw.fio_write = value & FIO_CTRL_WRITE;
    }
    if (addr == W5_COMMAND) {
        hw.command = value;
        if (value == W5_SLEEP_VPU) {
            if (dev->legacy_firmware)
                assert(hw.idle_seen && !hw.reenables);
            else
                assert(!hw.idle_seen);
            assert(!hw.reset_requests);
            hw.sleeps++;
        } else if (value == W5_WAKEUP_VPU) {
            assert(hw.reset_done && hw.reenables);
            hw.wakes++;
        }
    }
    if (addr == W5_VPU_RESET_REQ) {
        if (value) {
            /* No reset is allowed before an observed bus-idle result. */
            assert(hw.idle_seen);
            assert(!hw.reenables);
            hw.reset_requests++;
        } else if (!hw.reset_stuck) {
            hw.reset_done = true;
        }
    }
}
static u32 wave5_vdi_read_register(struct vpu_device *dev, u32 addr)
{
    (void)dev;
    if (addr == W5_VPU_RET_VPU_CONFIG0) return hw.config;
    if (addr == W5_VPU_RESET_STATUS) return hw.reset_stuck ? 1 : 0;
    int command_ret = hw.command == W5_SLEEP_VPU ? hw.sleep_ret :
                      hw.command == W5_WAKEUP_VPU ? hw.wake_ret : 0;
    if (addr == W5_VPU_BUSY_STATUS) return command_ret == -ETIMEDOUT;
    if (addr == W5_RET_SUCCESS) return !command_ret;
    if (addr == W5_RET_FAIL_REASON) return 2;
    if (addr == W5_VPU_FIO_CTRL_ADDR) {
        if (hw.fio_starts == hw.fail_fio_at) return 0;
        if (++hw.ctrl_reads <= hw.ready_delay) return 0;
        if (hw.fio_write) {
            if (is_bus_control(hw.address) && !hw.data) {
                assert(hw.reset_done);
                if (!hw.reenables) hw.enable_first = hw.fio_starts;
                hw.reenables++;
            }
            hw.indirect[hw.address / 4] = hw.data;
        }
        return FIO_CTRL_READY;
    }
    if (addr == W5_VPU_FIO_DATA) {
        bool idle = !hw.never_idle && hw.fio_data_reads++ >= hw.busy_reads;
        /* Combined and separate VCPU buses alias the same FIO address. */
        if ((hw.config & W521_FEATURE_VCPU_BACKBONE) &&
            hw.address == W5_BACKBONE_BUS_STATUS_VCPU)
            return idle ? 0 : 1;
        if (idle) hw.idle_seen = true;
        return idle ? dev->bus_idle_value : 0;
    }
    return 0;
}
#define vpu_write_reg wave5_vdi_write_register
#define vpu_read_reg wave5_vdi_read_register
static void remap_page(struct vpu_device *dev, dma_addr_t base, unsigned index)
{
    (void)dev; (void)base; (void)index;
}
static void setup_wave5_interrupts(struct vpu_device *dev) { (void)dev; }
'''
checks = r'''
static struct vpu_device vpu;
static struct device dev = { .data = &vpu };
static void setup(u32 config)
{
    memset(&hw, 0, sizeof(hw));
    memset(&vpu, 0, sizeof(vpu));
    hw.config = config;
    vpu.product_code = WAVE521C_CODE;
    vpu.bus_idle_value = 0x3f;
    vpu.reset_mask = GENMASK(26, 0);
    vpu.legacy_firmware = true;
}
int main(void)
{
    const u32 configs[] = { 0, W521_FEATURE_BACKBONE,
        W521_FEATURE_BACKBONE | W521_FEATURE_VCORE_BACKBONE,
        W521_FEATURE_BACKBONE | W521_FEATURE_VCORE_BACKBONE |
            W521_FEATURE_VCPU_BACKBONE };
    unsigned cases = 0;
    for (unsigned c = 0; c < sizeof(configs)/sizeof(configs[0]); c++) {
        /* The indirect read is ready before its bus has drained. */
        setup(configs[c]);
        hw.busy_reads = 3;
        hw.ready_delay = 2;
        int ret = wave5_vpu_reset(&dev, SW_RESET_ON_BOOT);
        if (ret)
            fprintf(stderr, "config=%#x ret=%d transactions=%u reads=%u time=%lld\n",
                    configs[c], ret, hw.fio_starts, hw.fio_data_reads, (long long)hw.now);
        assert(!ret);
        assert(hw.fio_data_reads > 3 && hw.reset_requests == 1);
        assert(hw.reenables && !hw.wakes);
        cases++;

        setup(configs[c]);
        hw.never_idle = true;
        assert(wave5_vpu_reset(&dev, SW_RESET_ON_BOOT) == -ETIMEDOUT);
        assert(!hw.reset_requests && !hw.reenables);
        assert(hw.now < FIO_TIMEOUT + 10000);
        cases++;

        /* Neither an unacknowledged stop nor a failed FIO read is idle. */
        unsigned stops = configs[c] & W521_FEATURE_VCPU_BACKBONE ? 2 : 1;
        for (unsigned fail = 1; fail <= stops * 2; fail++) {
            setup(configs[c]);
            hw.fail_fio_at = fail;
            assert(wave5_vpu_reset(&dev, SW_RESET_ON_BOOT) == -ETIMEDOUT);
            assert(!hw.reset_requests && !hw.reenables);
            assert(hw.now < FIO_TIMEOUT + 15000);
            cases++;
        }

        setup(configs[c]);
        hw.reset_stuck = true;
        assert(wave5_vpu_reset(&dev, SW_RESET_ON_BOOT) == -ETIMEDOUT);
        assert(hw.reset_requests == 1 && !hw.reenables && !hw.wakes);
        cases++;

        setup(configs[c]);
        hw.sleep_ret = -EIO;
        assert(wave5_vpu_reset(&dev, SW_RESET_SAFETY) == -EIO);
        assert(hw.sleeps == 1 && !hw.reset_requests && !hw.reenables);
        cases++;

        setup(configs[c]);
        assert(!wave5_vpu_reset(&dev, SW_RESET_SAFETY));
        assert(hw.sleeps == 1 && hw.wakes == 1);
        /* Propagate a failure of each bus-enable write after reset too. */
        unsigned enable_first = hw.enable_first;
        unsigned enables = hw.reenables;
        for (unsigned n = 0; n < enables; n++) {
            setup(configs[c]);
            hw.fail_fio_at = enable_first + n;
            assert(wave5_vpu_reset(&dev, SW_RESET_SAFETY) == -ETIMEDOUT);
            assert(hw.reset_done && !hw.wakes);
            cases++;
        }
        cases++;

        setup(configs[c]);
        hw.wake_ret = -EIO;
        assert(wave5_vpu_reset(&dev, SW_RESET_SAFETY) == -EIO);
        assert(hw.reset_done && hw.wakes == 1);
        cases++;
    }

    setup(0);
    vpu.legacy_firmware = false;
    assert(!wave5_vpu_reset(&dev, SW_RESET_SAFETY));
    assert(hw.sleeps == 1 && hw.wakes == 1);
    cases++;

    setup(0);
    assert(!wave5_vpu_reset(&dev, SW_RESET_FORCE));
    assert(!hw.sleeps && hw.wakes == 1);
    cases++;

    setup(0);
    hw.sleep_ret = -ETIMEDOUT;
    assert(wave5_vpu_reset(&dev, SW_RESET_SAFETY) == -ETIMEDOUT);
    assert(!hw.reset_requests && !hw.reenables);
    cases++;

    setup(0);
    hw.wake_ret = -ETIMEDOUT;
    assert(wave5_vpu_reset(&dev, SW_RESET_SAFETY) == -ETIMEDOUT);
    assert(hw.reset_done && hw.wakes == 1);
    cases++;

    /* Repeated indirect transactions share one deadline, including READY. */
    setup(0);
    hw.busy_reads = 1000000;
    hw.ready_delay = 100;
    assert(wave5_wait_bus_busy(&vpu, W5_GDI_BUS_STATUS) == -ETIMEDOUT);
    assert(hw.fio_starts > 1 && hw.now < FIO_TIMEOUT + 10000);
    cases++;

    setup(0);
    u32 data = 0x12345678;
    assert(wave5_fio_readl(&vpu, W5_GDI_BUS_STATUS, &data, hw.now) == -ETIMEDOUT);
    assert(data == 0x12345678 && !hw.writes);
    cases++;

    setup(0);
    assert(wave5_vpu_reset(&dev, 99) == -EINVAL && !hw.writes);
    vpu.reset_mask = 0;
    assert(wave5_vpu_reset(&dev, SW_RESET_ON_BOOT) == -EINVAL && !hw.writes);
    cases += 2;

    printf("Wave5 FIO/reset: %u deterministic MMIO fault/order cases passed\n", cases);
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-reset-') as directory:
    directory = Path(directory)
    # Keep the real kernel polling macro, mocking only its environment.
    headers = directory / 'linux'
    headers.mkdir()
    for name in ('kernel', 'types', 'ktime', 'delay', 'io'):
        (headers / f'{name}.h').write_text('/* Defined by the harness. */\n')
    iopoll = root.parents[4] / 'include/linux/iopoll.h'
    code = directory / 'reset.c'
    binary = directory / 'reset'
    code.write_text(prefix + defines + '\n' +
                    f'#include "{iopoll}"\n#include "{root / "wave5-regdefine.h"}"\n' +
                    f'#include "{root / "wave5-vpuconfig.h"}"\n' +
                    mmio + polling + command_check + sleep_wake + reset + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-I', str(directory),
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
