#!/usr/bin/env python3
"""Run actual snapshot IRQ/capture code against bounded MMIO and failure schedules."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
source = source.replace('#include "t7-vdin-hdr.h"', Path(__file__).with_name('t7-vdin-hdr.h').read_text())
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
#define VDIN_FRAME_SIZE (1920U*1080U*3U)
#define VDIN_STRIDE (1920U*3U)
#define __iomem
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
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
struct delayed_work { int unused; };
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
static uint8_t registers[0x40000], frame[3840U * 2160U * 3U + 16];
static struct t7_vdin receiver;
static struct t7_vdin_buffer *t7_vdin_next_buffer(struct t7_vdin *vdin)
{ assert(!vdin->direct_dma); return NULL; }
static unsigned int pins, requests, timing_checks, receiver_users;
static unsigned int pq_writes, gain_writes, oetf_writes;
static bool hdr_input, yuv_input;
static bool t7_hdmirx_stream_is_hdr(struct device *dev) { (void)dev; return hdr_input; }
static bool t7_hdmirx_stream_is_yuv(struct device *dev) { (void)dev; return hdr_input || yuv_input; }
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
    if (address == registers + 0x129f * 4) pq_writes++;
    if (address == registers + 0x12a7 * 4) gain_writes++;
    if (address == registers + 0x12a1 * 4) oetf_writes++;
    assert((uint8_t *)address >= registers &&
           (uint8_t *)address + 4 <= registers + sizeof(registers));
    if (address == registers + VDIN_WR_CTRL * 4 && value & VDIN_WR_REQ) {
        /* Every format must feed the MIF, not just program its addresses. */
        assert(readl(registers + VDIN_COM_CTRL0 * 4) & BIT(4));
        /* Never start DMA before its bounded address/stride have been installed. */
        assert(readl(registers + VDIN_WR_BADDR * 4) == receiver.frame_dma >> 4);
        assert(readl(registers + VDIN_WR_STRIDE * 4) == receiver.stride >> 4);
        if (receiver.nv12) {
            assert(readl(registers + VDIN_WR_CHROMA * 4) ==
                   (receiver.frame_dma + receiver.stride * receiver.height) >> 4);
            assert(receiver.frame_size == receiver.stride * receiver.height * 3 / 2);
        }
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
static int t7_hdmirx_check_video(struct device *dev)
{
    (void)dev;
    timing_checks++;
    return bad_timing || (change_timing && timing_checks == 2) ? -ERANGE : 0;
}
static int t7_hdmirx_stream_get(struct device *dev, unsigned int width, unsigned int height)
{
    assert(width==receiver.input_width && height==receiver.input_height);
    int ret = t7_hdmirx_check_video(dev);
    if (!ret) receiver_users++;
    return ret;
}
static void t7_hdmirx_stream_put(struct device *dev)
{
    (void)dev;
    assert(receiver_users && !receiver.armed && receiver.dma_idle);
    receiver_users--;
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
    memset(frame, 0x5a, receiver.frame_size);
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
    memcpy(receiver.hdr_gain,t7_hdr_gain,sizeof(t7_hdr_gain));
    memcpy(receiver.hdr_oetf,t7_hdr_srgb,sizeof(t7_hdr_srgb));
    memset(registers, 0, sizeof(registers));
    memset(frame, 0xa5, sizeof(frame));
    receiver = (struct t7_vdin) { .regs = registers, .frame = frame,
                               .frame_dma = 0x10000000, .width = 1920, .height = 1080,
                               .input_width = 1920, .input_height = 1080,
                               .stride = VDIN_STRIDE, .frame_size = VDIN_FRAME_SIZE };
    pins = requests = timing_checks = receiver_users = 0;
    timeout = busy = bad_timing = change_timing = false;
}
int main(void)
{
    reset();
    writel(0xc0000000, registers + VDIN_LFIFO_CTRL * 4);
    assert(!t7_vdin_capture(&receiver));
    assert((readl(registers + VDIN_LFIFO_CTRL * 4) & 0xc0000000) == 0xc0000000);
    assert(receiver.snapshots == 1 && receiver.frame_irqs == 2 && !receiver.armed);
    assert(!receiver_users);
    assert(requests == 1 && timing_checks == 2 && frame[receiver.frame_size - 1] == 0x5a);
    assert(frame[receiver.frame_size] == 0xa5);
    assert(!t7_vdin_capture(&receiver) && receiver.snapshots == 2);
    reset(); receiver.nv12 = true;
    receiver.width = 3840; receiver.height = 2160; receiver.stride = 3840;
    receiver.input_width = 3840; receiver.input_height = 2160;
    receiver.frame_size = 3840U * 2160U * 3U / 2;
    assert(!t7_vdin_capture(&receiver));
    assert(frame[receiver.frame_size - 1] == 0x5a && frame[receiver.frame_size] == 0xa5);
    /* T7's physical FIFO is 1920 words, independently of the input width. */
    assert((readl(registers + VDIN_LFIFO_CTRL * 4) & 0x3fff) == 0x780);
    assert((readl(registers + VDIN_WR_CTRL * 4) >> 12 & 3) == 2);
    assert(readl(registers + VDIN_WR_CHROMA_STRIDE * 4) == 3840U / 16);
    /* Full HDMI input feeds a smaller DMA image; switching back disables scaling. */
    receiver.width=1920; receiver.height=1080; receiver.stride=1920;
    receiver.frame_size=1920U*1080U*3/2;
    assert(!t7_vdin_capture(&receiver));
    assert(readl(registers+VDIN_WIDTHS*4)==((3839U<<16)|1919));
    assert(readl(registers+VDIN_HSC_PHASE_STEP*4)==2U<<24);
    assert(readl(registers+VDIN_VSC_PHASE_STEP*4)==2U<<20);
    assert(readl(registers+VDIN_VSC_INI_CTRL*4)&BIT(23));
    receiver.width=3840; receiver.height=2160; receiver.stride=3840;
    receiver.frame_size=3840U*2160U*3/2;
    assert(!t7_vdin_capture(&receiver));
    assert(!readl(registers+VDIN_SC_MISC_CTRL*4));
    assert(!readl(registers+VDIN_VSC_INI_CTRL*4));
    reset(); receiver.nv12=true;hdr_input=true;
    t7_vdin_configure(&receiver);
    assert(pq_writes==143 && gain_writes==75 && oetf_writes==75);
    assert(readl(registers+0x12b0*4)==0x18);
    assert(!(readl(registers+VDIN_HDR_CTRL*4)&BIT(16)));
    assert(readl(registers+VDIN_HDR_MATRIX_OUT*4)==1);
    hdr_input=false;yuv_input=true;t7_vdin_configure(&receiver);
    assert(readl(registers+VDIN_HDR_CTRL*4)==0);
    assert(readl(registers+VDIN_MATRIX_CTRL*4)==0);
    yuv_input=false;t7_vdin_configure(&receiver);
    assert(readl(registers+VDIN_HDR_CTRL*4)==BIT(16));
    assert(readl(registers+VDIN_HDR_MATRIX_OUT*4)==0);
    reset(); bad_timing = true;
    assert(t7_vdin_capture(&receiver) == -ERANGE && !requests && !receiver.armed);
    reset(); timeout = true;
    assert(t7_vdin_capture(&receiver) == -ETIMEDOUT);
    assert(!receiver.armed && receiver.faulted && pins == 1 && !receiver.snapshots);
    assert(receiver_users == 1);
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
