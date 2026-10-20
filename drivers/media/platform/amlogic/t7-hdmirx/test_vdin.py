#!/usr/bin/env python3
"""Run actual snapshot IRQ/capture code against bounded MMIO and failure schedules."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
types = source[source.index('enum t7_vdin_slot_state {'):source.index('static u32 t7_vdin_read')]
functions = source[source.index('static u32 t7_vdin_read'):
                   source.index('static int t7_vdin_frame_open')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
typedef uint64_t u64;
typedef int spinlock_t;
typedef int wait_queue_head_t;
typedef int irqreturn_t;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define IRQ_HANDLED 1
#define THIS_MODULE ((void *)0)
#define spin_lock_irqsave(lock, flags) ((void)(lock), (void)((flags) = 0))
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define dma_wmb() ((void)0)
#define dma_rmb() ((void)0)
#define dev_err(...) ((void)0)
#define msecs_to_jiffies(ms) (ms)
struct device { int unused; };
struct clk_bulk_data { int unused; };
struct mutex { int unused; };
struct completion { bool done; };
struct kref { int unused; };
struct list_head { int unused; };
struct vb2_v4l2_buffer { int unused; };
struct v4l2_device { int unused; };
struct video_device { int unused; };
struct vb2_queue { int unused; };
static u64 ktime_get_ns(void) { return 123456789; }
static void wake_up(wait_queue_head_t *w) { (void)w; }
'''
mmio = r'''
static uint8_t registers[0x40000], frame[VDIN_FRAME_SIZE];
static struct t7_vdin receiver;
static unsigned int pins, requests, timing_checks;
static bool timeout, busy, bad_timing, change_timing;
static u32 readl(void *address)
{
    u32 value;
    assert((uint8_t *)address >= registers &&
           (uint8_t *)address + 4 <= registers + sizeof(registers));
    memcpy(&value, address, 4);
    return value;
}
static void writel(u32 value, void *address)
{
    assert((uint8_t *)address >= registers &&
           (uint8_t *)address + 4 <= registers + sizeof(registers));
    if (address == registers + VDIN_WR_CTRL * 4 && value & VDIN_WR_REQ) {
        /* Never start DMA before its bounded address/stride have been installed. */
        assert(readl(registers + VDIN_WR_BADDR * 4) == receiver.frame_dma >> 4);
        assert(readl(registers + VDIN_WR_STRIDE * 4) == VDIN_STRIDE >> 4);
        if (!(readl(address) & VDIN_WR_REQ)) requests++;
    }
    memcpy(address, &value, 4);
}
static void __module_get(void *module) { (void)module; pins++; }
static void synchronize_irq(int irq) { (void)irq; }
#define readl_poll_timeout(address, value, condition, interval, limit) \
    ((value = readl(address)), (condition) ? 0 : -ETIMEDOUT)
static void complete(struct completion *done)
{
    assert(!receiver.armed);
    assert(!(readl(registers + VDIN_WR_CTRL * 4) & (VDIN_WR_REQ | VDIN_VCP_WR)));
    done->done = true;
}
static void reinit_completion(struct completion *done) { done->done = false; }
static void udelay(unsigned int delay) { assert(delay == 1); }
static int t7_hdmirx_check_1080p(struct device *dev)
{
    (void)dev;
    timing_checks++;
    return bad_timing || (change_timing && timing_checks == 2) ? -ERANGE : 0;
}
static unsigned long wait_for_completion_timeout(struct completion *done, unsigned long limit);
'''
checks = r'''
static unsigned long wait_for_completion_timeout(struct completion *done, unsigned long limit)
{
    assert(limit == 1000);
    /* Without per-line packer reset or IRQ routing, hardware never completes. */
    if (!(readl(registers + VDIN_WR_CTRL * 4) & BIT(27)) ||
        !(readl(registers + VDIN_DOUBLE_CTRL * 4) & BIT(20))) return 0;
    if (timeout) return 0;
    assert(receiver.armed);
    t7_vdin_write_done(1, &receiver);
    assert(receiver.armed && receiver.frame_irqs == 1 && !done->done);
    assert(readl(registers + VDIN_WR_CTRL * 4) & VDIN_WR_REQ);
    memset(frame, 0x5a, sizeof(frame));
    if (!busy) writel(BIT(0), registers + VDIN_WR_STATUS * 4);
    t7_vdin_write_done(1, &receiver);
    assert(!receiver.armed && receiver.frame_irqs == 2 && done->done);
    t7_vdin_write_done(1, &receiver);
    assert(receiver.frame_irqs == 2);
    /* Linux consumes one completion count when a waiter succeeds. */
    done->done = false;
    return 1;
}
static void reset(void)
{
    memset(registers, 0, sizeof(registers));
    receiver = (struct t7_vdin) { .regs = registers, .frame = frame,
                               .frame_dma = 0x10000000 };
    pins = requests = timing_checks = 0;
    timeout = busy = bad_timing = change_timing = false;
}
int main(void)
{
    reset();
    writel(0xc0000000, registers + VDIN_LFIFO_CTRL * 4);
    assert(!t7_vdin_capture(&receiver));
    assert((readl(registers + VDIN_LFIFO_CTRL * 4) & 0xc0000000) == 0xc0000000);
    assert(receiver.snapshots == 1 && receiver.frame_irqs == 2 && !receiver.armed);
    assert(requests == 1 && timing_checks == 2 && frame[sizeof(frame) - 1] == 0x5a);
    assert(!t7_vdin_capture(&receiver) && receiver.snapshots == 2);
    reset(); bad_timing = true;
    assert(t7_vdin_capture(&receiver) == -ERANGE && !requests && !receiver.armed);
    reset(); timeout = true;
    assert(t7_vdin_capture(&receiver) == -ETIMEDOUT);
    assert(!receiver.armed && receiver.faulted && pins == 1 && !receiver.snapshots);
    assert(!(readl(registers + VDIN_WR_CTRL * 4) & VDIN_WR_REQ));
    reset(); change_timing = true;
    assert(t7_vdin_capture(&receiver) == -ERANGE && !receiver.snapshots);
    reset(); busy = true;
    assert(t7_vdin_capture(&receiver) == -ETIMEDOUT && receiver.faulted && pins == 1);
    requests = 0;
    assert(t7_vdin_capture(&receiver) == -EIO && !requests && pins == 1);
    assert(t7_vdin_stop_dma(&receiver) == -ETIMEDOUT && pins == 1);
    puts("VDIN completion ordering, DMA bounds, timing change and timeout containment passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'vdin.c'
    executable = Path(directory) / 'vdin'
    code.write_text(prefix + defines + '\n' + types + mmio + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
