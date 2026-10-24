#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual driver functions against deterministic failing backends.

No hardware, kernel module loading, duplicated stop implementation or sleeps.
The backends inject every MMIO failure, stuck status and ownership transition.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def function(path, name):
    source = path.read_text()
    # Function signatures used here fit on one line.
    start = source.rfind("\n", 0, source.index(name + "(")) + 1
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def run_c(source):
    with tempfile.TemporaryDirectory(prefix="t7-vdec-test-") as directory:
        path = Path(directory) / "test.c"
        path.write_text(source)
        output = Path(directory) / "test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror",
                        "-Wno-unused-function", "-I", str(HERE),
                        str(path), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True, timeout=5)


COMMON = """
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#define BIT(n) (1U << (n))
typedef uint32_t u32;
"""


class LifetimeTests(unittest.TestCase):
    def test_m2m_job_binds_codec_only_until_dma_stop(self):
        worker = function(HERE / "aml_vdec.c", "m2mops_vdec_device_run")
        run_c(COMMON + """
#define WRITE_ONCE(p, value) ((p) = (value))
#define WARN_ON_ONCE(v) (v)
#define dev_dbg(...) ((void)0)
#define VB2_BUF_STATE_DONE 1
#define VB2_BUF_STATE_ERROR 2
struct media_request {};
struct vb2_v4l2_buffer { struct { int index; struct { struct media_request *req; } req_obj; } vb2_buf; };
struct aml_vdec_ctx;
struct codec_ops { int (*run)(struct aml_vdec_ctx *); };
struct hw { bool faulted; void *curr_ctx; };
struct platform_data { const char *fw_path[1]; };
struct aml_vdec_dev { struct hw *dec_hw; struct platform_data *pvdec_data; int m2m_dev_dec; };
struct aml_vdec_ctx {
    struct aml_vdec_dev *dev;
    struct { void *m2m_ctx; } fh;
    const struct codec_ops *codec_ops;
    int curr_dec_type, ctrl_handler, dos_clk_en;
    void *codec_priv, *m2m_ctx;
};
static struct vb2_v4l2_buffer buffer;
static int jobs, stops, firmware_error;
static bool stop_error;
static struct aml_vdec_ctx *owner;
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(void *p) { return &buffer; }
static struct vb2_v4l2_buffer *v4l2_m2m_next_dst_buf(void *p) { return &buffer; }
static int v4l2_ctrl_request_setup(void *p, int *h) { return 0; }
static void v4l2_ctrl_request_complete(void *p, int *h) {}
static void dos_enable(struct hw *h) { assert(h->curr_ctx == owner->codec_priv); }
static int aml_vdec_reset_core(struct hw *h) { return 0; }
static int load_firmware(struct hw *h, const char *path)
{ assert(h->curr_ctx == owner->codec_priv); return firmware_error; }
static void aml_vdec_fault(struct aml_vdec_ctx *ctx) { ctx->dev->dec_hw->faulted = true; }
static void aml_vdec_stop_job(struct aml_vdec_ctx *ctx)
{
    if (!ctx->dos_clk_en || ctx->dev->dec_hw->faulted) return;
    assert(ctx == owner && ctx->dev->dec_hw->curr_ctx == ctx->codec_priv);
    stops++;
    if (stop_error) aml_vdec_fault(ctx);
    else ctx->dos_clk_en = 0;
}
static void v4l2_m2m_buf_copy_metadata(void *src, void *dst) {}
static void v4l2_m2m_buf_done_and_job_finish(int device, void *ctx, int result)
{
    assert(ctx == owner->m2m_ctx);
    assert(owner->dev->dec_hw->faulted || !owner->dev->dec_hw->curr_ctx);
    assert(result == ((firmware_error || stop_error) ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE));
    jobs++;
}
static int decode(struct aml_vdec_ctx *ctx)
{ assert(ctx == owner && ctx->dev->dec_hw->curr_ctx == ctx->codec_priv); return 0; }
""" + worker + """
int main(void)
{
    struct hw hw = {};
    struct platform_data data = { { "h264" } };
    struct aml_vdec_dev dev = { &hw, &data };
    struct codec_ops ops = { decode };
    struct aml_vdec_ctx a = { .dev = &dev, .codec_ops = &ops, .codec_priv = &a, .m2m_ctx = &a };
    struct aml_vdec_ctx b = { .dev = &dev, .codec_ops = &ops, .codec_priv = &b, .m2m_ctx = &b };
    owner = &a; m2mops_vdec_device_run(&a);
    owner = &b; m2mops_vdec_device_run(&b);
    owner = &a; m2mops_vdec_device_run(&a);
    assert(jobs == 3 && stops == 3 && !hw.curr_ctx);
    firmware_error = -EIO;
    owner = &b; m2mops_vdec_device_run(&b);
    assert(jobs == 4 && stops == 4 && !hw.curr_ctx);
    firmware_error = 0; stop_error = true;
    owner = &a; m2mops_vdec_device_run(&a);
    assert(jobs == 5 && stops == 5 && hw.faulted && hw.curr_ctx == a.codec_priv);
    return 0;
}
""")

    def test_multiple_files_keep_independent_queues_and_pm_references(self):
        functions = "\n".join(function(HERE / "aml_vdec_drv.c", name) for name in (
            "fops_vcodec_open", "fops_vcodec_release"))
        run_c(COMMON + """
#include <stdlib.h>
#define READ_ONCE(x) (x)
#define GFP_KERNEL 0
#define __force
#define kzalloc_obj(obj, flags) calloc(1, sizeof(obj))
#define kfree free
#define IS_ERR(p) ((p) == (void *)-ENOMEM)
#define PTR_ERR(p) (-ENOMEM)
#define v4l2_err(...) ((void)0)
struct aml_vdec_hw { bool faulted, power_test_passed, mdec_test_passed; void *curr_ctx; };
struct platform { int dev; };
struct platform_data { bool has_dbus_ctrl; };
struct aml_vdec_dev {
    struct aml_vdec_hw *dec_hw;
    struct platform *plat_dev;
    struct platform_data *pvdec_data;
    unsigned int open_count;
    int dev_mutex, m2m_dev_dec;
};
struct fh { void *m2m_ctx; };
struct aml_vdec_ctx {
    struct aml_vdec_dev *dev;
    struct fh fh;
    void *m2m_ctx;
    int v4l2_intf_lock, queue, int_cond, ctrl_handler;
};
struct file { void *private_data; };
static struct aml_vdec_hw hw = { .power_test_passed = true, .mdec_test_passed = true };
static struct platform platform;
static struct platform_data data = { true };
static struct aml_vdec_dev dev = { .dec_hw = &hw, .plat_dev = &platform, .pvdec_data = &data };
static bool t7_snapshot_only, allow_t7_power = true, fail_queue, fail_controls;
static int pm_refs, contexts, handles;
static struct aml_vdec_dev *video_drvdata(struct file *f) { return &dev; }
static void *video_devdata(struct file *f) { return NULL; }
static struct aml_vdec_ctx *fh_to_dec_ctx(struct file *f)
{ return (void *)((char *)f->private_data - offsetof(struct aml_vdec_ctx, fh)); }
static void mutex_lock(int *p) {}
static void mutex_unlock(int *p) {}
static void mutex_init(int *p) {}
static void init_waitqueue_head(int *p) {}
static int pm_runtime_resume_and_get(int *p) { return ++pm_refs, 0; }
static int pm_runtime_put_sync_suspend(int *p) { assert(pm_refs > 0); return --pm_refs, 0; }
static void v4l2_fh_init(struct fh *fh, void *vfd) { handles++; }
static void v4l2_fh_add(struct fh *fh, struct file *f) {}
static void v4l2_fh_del(struct fh *fh, struct file *f) {}
static void v4l2_fh_exit(struct fh *fh) { handles--; }
static void aml_vdec_queue_init(void) {}
static void *v4l2_m2m_ctx_init(int device, void *ctx, void (*init)(void))
{ if (fail_queue) return (void *)-ENOMEM; contexts++; return malloc(1); }
static void v4l2_m2m_ctx_release(void *ctx) { contexts--; free(ctx); }
static int aml_vdec_ctrls_setup(struct aml_vdec_ctx *ctx) { return fail_controls ? -ENOMEM : 0; }
static void aml_vdec_reset_fmts(struct aml_vdec_ctx *ctx) {}
static void v4l2_ctrl_handler_free(int *p) {}
static void aml_vdec_fault(struct aml_vdec_ctx *ctx) { assert(!"unexpected PM failure"); }
""" + functions + """
int main(void)
{
    struct file first = {}, second = {}, failed = {};
    struct aml_vdec_ctx *a, *b;

    assert(!fops_vcodec_open(&first));
    a = fh_to_dec_ctx(&first);
    hw.curr_ctx = a; /* Another file may open/close during a running job. */
    assert(!fops_vcodec_open(&second));
    b = fh_to_dec_ctx(&second);
    assert(a != b && a->m2m_ctx != b->m2m_ctx);
    assert(a->fh.m2m_ctx == a->m2m_ctx && b->fh.m2m_ctx == b->m2m_ctx);
    assert(dev.open_count == 2 && pm_refs == 2 && contexts == 2 && handles == 2);
    fail_queue = true;
    assert(fops_vcodec_open(&failed) == -ENOMEM);
    fail_queue = false;
    fail_controls = true;
    assert(fops_vcodec_open(&failed) == -ENOMEM);
    fail_controls = false;
    assert(dev.open_count == 2 && pm_refs == 2 && contexts == 2 && handles == 2);
    assert(hw.curr_ctx == a);
    assert(!fops_vcodec_release(&second));
    assert(dev.open_count == 1 && pm_refs == 1 && contexts == 1 && handles == 1);
    assert(hw.curr_ctx == a && a->m2m_ctx);
    hw.faulted = true;
    assert(fops_vcodec_open(&failed) == -EIO);
    hw.faulted = false;
    hw.curr_ctx = NULL;
    assert(!fops_vcodec_release(&first));
    assert(!dev.open_count && !pm_refs && !contexts && !handles);
    return 0;
}
""")

    def test_snapshot_guard_stops_internal_access_but_allows_abort(self):
        functions = "\n".join(function(HERE / "aml_vdec_drv.c", name) for name in (
            "register_step_store", "mdec_test_store"))
        run_c(COMMON + """
#include <stdlib.h>
#include <sys/types.h>
#define READ_ONCE(x) (x)
struct device {};
struct device_attribute {};
struct aml_vdec_hw { unsigned int probe_step; };
struct aml_vdec_dev { unsigned int open_count; int dev_mutex; struct aml_vdec_hw *dec_hw; };
static struct aml_vdec_hw hw;
static struct aml_vdec_dev vdec = { .dec_hw = &hw };
static bool t7_snapshot_only = true, allow_t7_power = true;
static unsigned int accesses, last_step;
static void *dev_get_drvdata(struct device *dev) { return &vdec; }
static int kstrtouint(const char *s, int base, unsigned int *v)
{ *v = strtoul(s, NULL, base); return 0; }
static int kstrtobool(const char *s, bool *v) { *v = true; return 0; }
static void mutex_lock(int *p) {}
static void mutex_unlock(int *p) {}
static bool pm_runtime_suspended(struct device *dev) { return true; }
static int aml_vdec_t7_register_step(struct aml_vdec_hw *p, unsigned int step)
{ accesses++; last_step = step; return 0; }
static int aml_vdec_t7_mdec_test(struct aml_vdec_hw *p)
{ assert(!"snapshot called the full MDEC sequence"); return 0; }
""" + functions + """
int main(void)
{
    assert(register_step_store(NULL, NULL, "8", 1) == 1 && accesses == 1);
    assert(register_step_store(NULL, NULL, "9", 1) == -EPERM && accesses == 1);
    assert(register_step_store(NULL, NULL, "13", 2) == -EPERM && accesses == 1);
    assert(mdec_test_store(NULL, NULL, "1", 1) == -EPERM && accesses == 1);
    allow_t7_power = false;
    assert(register_step_store(NULL, NULL, "0", 1) == 1);
    assert(accesses == 2 && last_step == 0);
    return 0;
}
""")

    def test_apb_clocks_are_held_before_dos_and_unwound_on_failure(self):
        functions = "\n".join(function(HERE / "aml_vdec_hw.c", name) for name in (
            "t7_clocks_on", "t7_clocks_off"))
        run_c(COMMON + """
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct aml_vdec_hw { int apb_gates[2], gates[2]; bool clocks_held, apb_held; };
static struct aml_vdec_hw hw;
static int held, attempts, fail_at;
static int clk_bulk_prepare_enable(int n, void *gates)
{
    assert(n == 2 && gates == (held ? hw.gates : hw.apb_gates));
    if (++attempts == fail_at) return -EIO;
    held++; return 0;
}
static void clk_bulk_disable_unprepare(int n, void *gates)
{
    assert(n == 2 && held && gates == (held == 2 ? hw.gates : hw.apb_gates));
    held--;
}
""" + functions + """
int main(void)
{
    for (fail_at = 1; fail_at <= 2; fail_at++) {
        attempts = 0;
        assert(t7_clocks_on(&hw) == -EIO && !held && !hw.clocks_held && !hw.apb_held);
        t7_clocks_off(&hw);
        assert(!held);
    }
    fail_at = attempts = 0;
    assert(!t7_clocks_on(&hw) && held == 2 && hw.clocks_held && hw.apb_held);
    t7_clocks_off(&hw);
    assert(!held && !hw.clocks_held && !hw.apb_held);
    t7_clocks_off(&hw);
    return 0;
}
""")

    def test_vendor_path_selection_propagates_errors_before_rate_programming(self):
        run_c(COMMON + """
#define VDEC_MUX 1
struct clk_bulk_data { void *clk; };
struct aml_vdec_hw { struct clk_bulk_data gates[2]; void *vdec_p0; };
static int parent_error, rate_error, parent_calls, rate_calls;
static int clk_set_parent(void *clk, void *parent)
{
    assert(clk == (void *)1 && parent == (void *)2);
    parent_calls++; return parent_error;
}
static int clk_set_rate(void *clk, int rate)
{
    assert(parent_calls && !parent_error && clk == (void *)2 && rate == 800000000);
    rate_calls++; return rate_error;
}
""" + function(HERE / "aml_vdec_hw.c", "t7_set_clock") + """
int main(void)
{
    struct aml_vdec_hw hw = {.gates[1].clk = (void *)1, .vdec_p0 = (void *)2};
    parent_error = -EIO;
    assert(t7_set_clock(&hw) == -EIO && parent_calls == 1 && !rate_calls);
    parent_error = 0; rate_error = -ERANGE;
    assert(t7_set_clock(&hw) == -ERANGE && parent_calls == 2 && rate_calls == 1);
    rate_error = 0;
    assert(!t7_set_clock(&hw) && parent_calls == 3 && rate_calls == 2);
    return 0;
}
""")

    def test_physical_clock_guard_rejects_closed_gates_and_wrong_recipe(self):
        run_c(COMMON + """
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
struct aml_vdec_hw { void *dev, *clkctrl; };
static u32 values[4];
static int reads, fail_at;
static int regmap_read(void *map, int reg, u32 *value)
{
    static const int offsets[] = {0x44, 0x4c, 0x140, 0x148};
    assert(reads < 4 && reg == offsets[reads]);
    *value = values[reads++];
    return fail_at == reads ? -EIO : 0;
}
""" + function(HERE / "aml_vdec_hw.c", "t7_clock_preflight") + """
int main(void)
{
    struct aml_vdec_hw hw = {0};
    /* Captured vendor active state, including independent HCODEC selection. */
    values[0] = 0xffffffff; values[1] = BIT(19) | BIT(20);
    values[2] = 0x100; values[3] = 0x80000000;
    assert(!t7_clock_preflight(&hw) && reads == 4);
    values[0] &= ~BIT(1); reads = 0;
    assert(t7_clock_preflight(&hw) == -EHOSTDOWN);
    values[0] |= BIT(1);
    for (int bit = 19; bit <= 20; bit++) {
        values[1] ^= BIT(bit); reads = 0;
        assert(t7_clock_preflight(&hw) == -EHOSTDOWN);
        values[1] ^= BIT(bit);
    }
    /* Gate off, alternate parent, or divider are insufficient at 800 MHz. */
    const u32 bad[] = {0, 0x300, 0x101};
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        values[2] = bad[i]; reads = 0;
        assert(t7_clock_preflight(&hw) == -EHOSTDOWN && reads == 4);
    }
    /* An enabled unselected p0 cannot hide a disabled selected p1. */
    values[2] = 0x7fff0100; values[3] |= BIT(15); reads = 0;
    assert(t7_clock_preflight(&hw) == -EHOSTDOWN);
    values[2] = 0; values[3] |= BIT(8); reads = 0;
    assert(t7_clock_preflight(&hw) == -EHOSTDOWN);
    values[2] = 0x7fff0100; values[3] = 0x80000000; reads = 0;
    assert(!t7_clock_preflight(&hw));
    for (fail_at = 1; fail_at <= 4; fail_at++) {
        reads = 0;
        assert(t7_clock_preflight(&hw) == -EIO && reads == fail_at);
    }
    return 0;
}
""")

    def test_parent_is_owned_before_top_access_and_child_power_and_released_last(self):
        functions = "\n".join(function(HERE / "aml_vdec_hw.c", name) for name in (
            "t7_clocks_on", "t7_clocks_off", "t7_power_off", "t7_set_clock",
            "t7_probe_begin", "t7_probe_domain_on"))
        run_c(COMMON + """
typedef int32_t s32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define VDEC_1 0
#define VDEC_MUX 1
#define PWRC_T7_DOS_VDEC_ID 4
#define dev_info(...) ((void)0)
struct arm_smccc_res { unsigned long a0; };
struct clk_bulk_data { void *clk; };
struct pd { void *dev; };
struct pm { struct pd pd_data[1]; };
struct aml_vdec_hw {
    void *dev, *nic3_dev, *vdec_p0;
    struct pm *pm;
    struct clk_bulk_data gates[2];
    struct clk_bulk_data apb_gates[2];
    bool clocks_held, apb_held, nic3_held, powered, faulted;
};
static int clocks, parent_refs, child_refs, fail_parent, fail_child, fail_off;
static int apb_refs;
static void *apb_gates;
static int state = 1, secure_error;
static bool en0_programmed;
static int clk_bulk_prepare_enable(int n, void *gates)
{
    assert(n == 2 && !clocks);
    if (gates == apb_gates) { assert(!apb_refs); apb_refs++; }
    else { assert(apb_refs == 1); clocks++; }
    return 0;
}
static void clk_bulk_disable_unprepare(int n, void *gates)
{
    assert(n == 2 && apb_refs == 1);
    if (gates == apb_gates) {
        assert(!clocks && !parent_refs && !child_refs); apb_refs--;
    } else { assert(clocks == 1); clocks--; }
}
static int clk_set_rate(void *clk, int rate)
{ assert(en0_programmed && clocks && parent_refs && !child_refs); return 0; }
static int clk_set_parent(void *clk, void *parent)
{ assert(en0_programmed && clocks && parent_refs && !child_refs); return 0; }
static int pm_runtime_resume_and_get(void *dev)
{
    assert(clocks);
    if (dev == (void *)1) {
        if (fail_parent) return -EIO;
        assert(!parent_refs && !child_refs); parent_refs++;
    } else {
        if (fail_child) return -EIO;
        assert(en0_programmed && parent_refs && !child_refs);
        child_refs++; state = 0;
    }
    return 0;
}
static int pm_runtime_put_sync_suspend(void *dev)
{
    assert(!clocks && apb_refs == 1);
    if (dev == (void *)1) {
        assert(parent_refs == 1 && !child_refs); parent_refs--;
    } else {
        assert(child_refs == 1 && parent_refs == 1); child_refs--; state = 1;
    }
    return fail_off ? -EIO : 0;
}
static int aml_vdec_t7_power_state(struct aml_vdec_hw *hw, s32 *vdec, s32 *nic3)
{ assert(clocks && parent_refs); *vdec = state; *nic3 = 0; return 0; }
static int t7_domain_state(int id, s32 *value) { *value = state; return 0; }
static void t7_power_fault(struct aml_vdec_hw *hw) { hw->faulted = true; }
static int t7_clock_preflight(struct aml_vdec_hw *hw) { return 0; }
static void arm_smccc_smc(int cmd, int id, int policy, int a, int b, int c, int d,
                         int e, struct arm_smccc_res *res)
{
    assert((unsigned int)cmd == 0xb200000e && id == 13 && !policy);
    assert(en0_programmed && parent_refs && child_refs);
    res->a0 = secure_error;
}
""" + functions + """
int main(void)
{
    struct pm pm = { .pd_data = {{ .dev = (void *)2 }} };
    struct aml_vdec_hw hw = { .nic3_dev = (void *)1, .pm = &pm };
    apb_gates = hw.apb_gates;
    assert(!t7_probe_begin(&hw) && hw.clocks_held && hw.nic3_held && !hw.powered);
    en0_programmed = true;
    assert(!t7_probe_domain_on(&hw) && hw.powered);
    assert(!t7_power_off(&hw) && !clocks && !parent_refs && !child_refs);
    assert(!hw.powered && !hw.nic3_held && !hw.clocks_held && !hw.apb_held && !apb_refs);
    fail_parent = 1;
    assert(t7_probe_begin(&hw) == -EIO && !hw.nic3_held);
    assert(!t7_power_off(&hw) && !clocks && !parent_refs && !child_refs);
    fail_parent = 0; fail_child = 1;
    assert(!t7_probe_begin(&hw) && t7_probe_domain_on(&hw) == -EIO);
    assert(!t7_power_off(&hw) && !clocks && !parent_refs && !child_refs);
    fail_child = 0; secure_error = 1;
    assert(!t7_probe_begin(&hw) && t7_probe_domain_on(&hw) == -EACCES);
    assert(!t7_power_off(&hw) && !clocks && !parent_refs && !child_refs);
    secure_error = 0; fail_off = 1;
    assert(!t7_probe_begin(&hw) && !t7_probe_domain_on(&hw));
    assert(t7_power_off(&hw) == -EIO && hw.faulted);
    assert(!clocks && !parent_refs && !child_refs);
    return 0;
}
""")

    def test_mdec_single_read_requires_all_prerequisites_and_verified_power_off(self):
        functions = "\n".join(function(HERE / "aml_vdec_hw.c", name) for name in (
            "t7_register_access", "aml_vdec_t7_register_step", "aml_vdec_t7_mdec_test"))
        run_c(COMMON + """
#include "reg_defines.h"
#define DOS_BUS 0
#define dev_info(...) ((void)0)
#define mb() ((void)0)
#define THIS_MODULE NULL
struct aml_vdec_hw {
    bool has_dbus_ctrl, powered, power_test_passed, mdec_test_passed, faulted;
    bool probe_failed, probe_pinned;
    u32 mdec_test_value, probe_step, probe_value, probe_en0;
    void *dev, *map[1];
};
static int fail_at, bad_at, calls, pic_reads, on_error, domain_error, off_error, off_calls;
static int phase, pins, writes;
static void __module_get(void *module) { pins++; }
static void module_put(void *module) { assert(pins == 1); pins--; }
static const int registers[] = {
    DOS_GCLK_EN0, DOS_GCLK_EN0, DOS_GCLK_EN0, DOS_GCLK_EN0, DOS_SW_RESET0,
    DOS_MEM_PD_VDEC, GCLK_EN, GCLK_EN, GCLK_EN, MDEC_PIC_DC_CTRL
};
static int access_reg(int reg)
{
    assert(phase == (calls < 3 ? 1 : 2));
    assert(calls < 10 && registers[calls] == reg);
    calls++;
    if (reg == MDEC_PIC_DC_CTRL) pic_reads++;
    return fail_at == calls ? -EIO : 0;
}
static int regmap_read(void *map, int reg, u32 *value)
{
    int ret = access_reg(reg);
    *value = reg == DOS_GCLK_EN0 ? (calls == 1 ? 0x7fff000 : 0x7fff3ff) :
             reg == GCLK_EN && calls == 9 ? 0x3ff : 0;
    if (calls == bad_at) *value ^= 1;
    return ret;
}
static int regmap_write(void *map, int reg, u32 value)
{
    assert((reg == DOS_GCLK_EN0 && value == 0x7fff3ff) ||
           (reg == GCLK_EN && value == 0x3ff));
    writes++;
    return access_reg(reg);
}
static int t7_probe_begin(struct aml_vdec_hw *hw)
{ assert(!calls); phase = 1; return on_error; }
static int t7_probe_domain_on(struct aml_vdec_hw *hw)
{ assert(calls == 3); phase = 2; hw->powered = !domain_error; return domain_error; }
static int t7_power_off(struct aml_vdec_hw *hw)
{ hw->powered = false; phase = 0; off_calls++; return off_error; }
""" + functions + """
int main(void)
{
    struct aml_vdec_hw hw = { .has_dbus_ctrl = true };
    assert(aml_vdec_t7_mdec_test(&hw) == -EPERM && !calls && !off_calls);
    hw.power_test_passed = true;
    on_error = -EIO;
    assert(aml_vdec_t7_mdec_test(&hw) == -EIO && !calls && off_calls == 1 && !pins);
    on_error = 0;
    hw.probe_failed = false; domain_error = -EACCES; off_calls = 0;
    assert(aml_vdec_t7_mdec_test(&hw) == -EACCES);
    assert(calls == 3 && !pic_reads && off_calls == 1 && !pins && !hw.mdec_test_passed);
    domain_error = 0;
    for (int i = 1; i <= 10; i++) {
        hw.probe_failed = false;
        calls = pic_reads = off_calls = writes = 0; fail_at = i;
        assert(aml_vdec_t7_mdec_test(&hw) == -EIO);
        assert(calls == i && pic_reads == (i == 10) && off_calls == 1 && !pins);
        assert(writes == (i >= 8 ? 2 : i >= 2 ? 1 : 0));
        assert(!hw.mdec_test_passed && !hw.powered);
        assert(aml_vdec_t7_register_step(&hw, 1) == -EIO && calls == i);
    }
    fail_at = 0;
    const int checks[] = {3, 4, 5, 6, 9};
    for (unsigned int i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        calls = pic_reads = off_calls = 0; bad_at = checks[i];
        hw.probe_failed = false;
        assert(aml_vdec_t7_mdec_test(&hw) < 0);
        assert(!pic_reads && off_calls == 1 && !hw.mdec_test_passed);
    }
    bad_at = calls = pic_reads = off_calls = 0;
    hw.probe_failed = false;
    off_error = -EIO;
    assert(aml_vdec_t7_mdec_test(&hw) == -EIO && !hw.mdec_test_passed);
    calls = pic_reads = off_calls = off_error = 0;
    hw.probe_failed = false;
    assert(!aml_vdec_t7_mdec_test(&hw));
    assert(calls == 10 && pic_reads == 1 && off_calls == 1 && !pins);
    assert(hw.mdec_test_passed && !hw.powered && !hw.probe_step);
    /* No skipped write after a read failure; no replay or out-of-order step. */
    calls = off_calls = writes = 0;
    assert(aml_vdec_t7_register_step(&hw, 8) == -EINVAL && !calls);
    assert(!aml_vdec_t7_register_step(&hw, 1) && pins == 1 && !hw.mdec_test_passed);
    assert(!aml_vdec_t7_register_step(&hw, 2) && calls == 1 && !writes);
    assert(aml_vdec_t7_register_step(&hw, 4) == -EINVAL && calls == 1 && !writes);
    assert(!aml_vdec_t7_register_step(&hw, 0) && !pins && off_calls == 1);
    assert(!hw.mdec_test_passed && !hw.probe_step);
    return 0;
}
""")

    def test_runtime_resume_requires_opt_in_and_completed_power_test(self):
        resume = function(HERE / "aml_vdec_drv.c", "aml_vdec_runtime_resume")
        run_c(COMMON + """
#define READ_ONCE(value) (value)
struct device { int unused; };
struct hw { bool power_test_passed, mdec_test_passed; };
struct pdata { bool has_dbus_ctrl; };
struct aml_vdec_dev { struct hw *dec_hw; struct pdata *pvdec_data; };
static struct hw hw;
static struct pdata pdata = { .has_dbus_ctrl = true };
static struct aml_vdec_dev vdec = { .dec_hw = &hw, .pvdec_data = &pdata };
static bool allow_t7_power, t7_snapshot_only;
static int power_calls;
static void *dev_get_drvdata(struct device *dev) { return &vdec; }
static int vdec_enable(struct hw *hw) { power_calls++; return 0; }
""" + resume + """
int main(void)
{
    struct device dev = {0};
    assert(aml_vdec_runtime_resume(&dev) == -EPERM && !power_calls);
    allow_t7_power = true;
    assert(aml_vdec_runtime_resume(&dev) == -EPERM && !power_calls);
    hw.power_test_passed = true;
    assert(aml_vdec_runtime_resume(&dev) == -EPERM && !power_calls);
    hw.mdec_test_passed = true;
    t7_snapshot_only = true;
    assert(aml_vdec_runtime_resume(&dev) == -EPERM && !power_calls);
    t7_snapshot_only = false;
    assert(!aml_vdec_runtime_resume(&dev) && power_calls == 1);
    allow_t7_power = false;
    assert(aml_vdec_runtime_resume(&dev) == -EPERM && power_calls == 1);
    return 0;
}
""")

    def test_power_test_never_touches_dos_before_secure_domains_report_on(self):
        functions = "\n".join(function(HERE / "aml_vdec_hw.c", name) for name in (
            "t7_domain_state", "aml_vdec_t7_power_state", "t7_power_fault",
            "t7_power_off", "t7_set_clock", "t7_power_on", "aml_vdec_t7_power_test"))
        run_c(COMMON + """
#include "reg_defines.h"
typedef int32_t s32;
#ifndef EHOSTDOWN
#define EHOSTDOWN 112
#endif
#define DOS_BUS 0
#define VDEC_1 0
#define VDEC_MUX 1
#define PWRC_T7_DOS_VDEC_ID 4
#define PWRC_T7_NIC3_ID 39
#define THIS_MODULE NULL
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_crit(...) ((void)0)
#define xchg(ptr, val) ({ bool old = *(ptr); *(ptr) = val; old; })
struct arm_smccc_res { unsigned long a0; };
struct clk_bulk_data { void *clk; };
struct pd { void *dev; };
struct pm { struct pd pd_data[1]; };
struct aml_vdec_hw {
    bool has_dbus_ctrl, powered, faulted, power_test_passed, mdec_test_passed;
    bool nic3_held, clocks_held, probe_failed;
    u32 power_test_reset;
    void *dev, *map[1], *nic3_dev, *vdec_p0;
    struct clk_bulk_data gates[2];
    struct pm *pm;
};
static int vdec_state, nic3_state, pm_error, off_error, clock_error;
static int clocks, refs, mmio, poison, phase;
static void arm_smccc_smc(unsigned int cmd, unsigned int id, int a, int b,
                         int c, int d, int e, int f, struct arm_smccc_res *res)
{
    assert(cmd == 0x82000095);
    assert(id == 4 || id == 39);
    res->a0 = id == 4 ? vdec_state : nic3_state;
}
static int clk_set_rate(void *clk, int rate) { assert(rate == 800000000); return 0; }
static int clk_set_parent(void *clk, void *parent) { assert(!phase); return 0; }
static int clk_bulk_prepare_enable(int n, void *gates)
{ assert(phase == 0); phase = 1; clocks++; return 0; }
static void clk_bulk_disable_unprepare(int n, void *gates)
{ assert(clocks == 1); clocks--; phase = 3; }
static int t7_clocks_on(struct aml_vdec_hw *hw)
{ int ret = clk_bulk_prepare_enable(2, hw->gates); hw->clocks_held = !ret; return ret; }
static void t7_clocks_off(struct aml_vdec_hw *hw)
{ if (hw->clocks_held) clk_bulk_disable_unprepare(2, hw->gates); hw->clocks_held = false; }
static int pm_runtime_resume_and_get(void *dev)
{ assert(phase == 1); phase = 2; if (pm_error) return pm_error; refs++; return 0; }
static int pm_runtime_put_sync_suspend(void *dev)
{ assert(phase == 3); refs--; if (!off_error) vdec_state = 1; return off_error; }
static void __module_get(void *module) { poison++; }
static void get_device(void *dev) {}
static void meson_t7_dos_poison(void) {}
static int t7_clock_preflight(struct aml_vdec_hw *hw)
{ assert(phase == 2 && !vdec_state && !nic3_state); return clock_error; }
static int regmap_read(void *map, int reg, u32 *value)
{
    assert(phase == 2 && clocks == 1 && refs == 1);
    assert(vdec_state == 0 && nic3_state == 0 && reg == DOS_SW_RESET0);
    mmio++; *value = 0; return 0;
}
""" + functions + """
int main(void)
{
    struct pm pm = {0};
    const int bad[] = {1, -1, -EIO, 2};
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        for (int parent = 0; parent < 2; parent++) {
            struct aml_vdec_hw hw = { .has_dbus_ctrl = true, .pm = &pm };
            clocks = refs = mmio = poison = phase = 0;
            vdec_state = parent ? 0 : bad[i];
            nic3_state = parent ? bad[i] : 0;
            assert(aml_vdec_t7_power_test(&hw) < 0);
            assert(!mmio && !clocks && !refs && !hw.power_test_passed);
        }
    }
    struct aml_vdec_hw hw = { .has_dbus_ctrl = true, .pm = &pm };
    phase = mmio = 0; vdec_state = nic3_state = 0; clock_error = -EHOSTDOWN;
    assert(aml_vdec_t7_power_test(&hw) == -EHOSTDOWN);
    assert(!mmio && !clocks && !refs && !hw.power_test_passed);
    clock_error = 0;
    phase = mmio = 0; vdec_state = nic3_state = 0;
    assert(!aml_vdec_t7_power_test(&hw));
    assert(mmio == 1 && !clocks && !refs && hw.power_test_passed && !hw.powered);
    phase = mmio = 0; vdec_state = nic3_state = 0; pm_error = -EIO;
    assert(aml_vdec_t7_power_test(&hw) == -EIO && !mmio && !clocks && !refs);
    phase = mmio = 0; pm_error = 0; off_error = -EIO;
    assert(aml_vdec_t7_power_test(&hw) == -EIO && hw.faulted && poison == 1);
    assert(!hw.power_test_passed && !hw.powered);
    assert(aml_vdec_t7_power_test(&hw) == -EIO && mmio == 1 && poison == 1);
    return 0;
}
""")

    def test_canvas_allocation_failure_preserves_unallocated_sentinels(self):
        source = (HERE / "h264.c").read_text()
        start = source.index("struct h264_decode_buf_spec {")
        spec = source[start:source.index("};", start) + 2]
        allocate = function(HERE / "h264.c", "config_decode_canvas")
        release = function(HERE / "h264.c", "release_canvas_index")
        run_c(COMMON + """
#include "reg_defines.h"
typedef uint8_t u8;
typedef uintptr_t dma_addr_t;
#define DOS_BUS 0
#define MESON_CANVAS_WRAP_NONE 0
#define MESON_CANVAS_BLKMODE_LINEAR 0
#define MESON_CANVAS_ENDIAN_SWAP64 0
#define dev_dbg(...) ((void)0)
#define spec2canvas(spec) 0
struct aml_vdec_hw { void *canvas, *dev, *map[1]; };
static int allocations, fail_at, frees;
static int meson_canvas_alloc(void *canvas, u8 *index)
{
    if (++allocations == fail_at) return -ENOSPC;
    *index = allocations + 10; return 0;
}
static void meson_canvas_free(void *canvas, int index)
{ assert(index == 11 || index == 12); frees++; }
#define meson_canvas_config(...) ((void)0)
static int regmap_write(void *map, int reg, unsigned int value) { return 0; }
""" + spec + allocate + release + """
int main(void)
{
    struct aml_vdec_hw hw = {0};
    for (int fail = 1; fail <= 3; fail++) {
        struct h264_decode_buf_spec spec = {
            .y_canvas_index = -1, .u_canvas_index = -1, .v_canvas_index = -1
        };
        allocations = frees = 0; fail_at = fail;
        int result = config_decode_canvas(&hw, &spec, 20, 16);
        assert(result == (fail <= 2 ? -ENOSPC : 0));
        release_canvas_index(&hw, &spec);
        assert(spec.y_canvas_index == -1 && spec.u_canvas_index == -1);
        assert(frees == (fail == 1 ? 0 : fail == 2 ? 1 : 2));
        release_canvas_index(&hw, &spec);
        assert(frees == (fail == 1 ? 0 : fail == 2 ? 1 : 2));
    }
    return 0;
}
""")

    def test_platform_core_respects_explicit_domain_ownership(self):
        root = HERE.parents[4]
        probe = function(root / "drivers/base/platform.c", "platform_probe")
        run_c(COMMON + """
#define EPROBE_DEFER 517
#define PD_FLAG_ATTACH_POWER_ON 1
#define PD_FLAG_DETACH_POWER_OFF 2
#define unlikely(value) (value)
#define dev_warn(...) ((void)0)
struct platform_device { int unused; };
struct platform_driver {
    int (*probe)(struct platform_device *);
    bool driver_managed_pm, prevent_deferred_probe;
};
struct device { struct platform_driver *driver; void *of_node; };
static struct platform_device pdev;
#define to_platform_driver(driver) (driver)
#define to_platform_device(dev) (&pdev)
static int domain_calls, probe_calls, attach_error;
static int platform_probe_fail(struct platform_device *dev) { return 0; }
static int of_clk_set_defaults(void *node, bool force) { return 0; }
static int dev_pm_domain_attach(struct device *dev, int flags)
{ domain_calls++; return attach_error; }
static int driver_probe(struct platform_device *dev) { probe_calls++; return 0; }
""" + probe + """
int main(void)
{
    struct platform_driver drv = { .probe = driver_probe, .driver_managed_pm = true };
    struct device dev = { .driver = &drv };
    assert(!platform_probe(&dev) && !domain_calls && probe_calls == 1);
    drv.driver_managed_pm = false;
    assert(!platform_probe(&dev) && domain_calls == 1 && probe_calls == 2);
    attach_error = -EPROBE_DEFER;
    assert(platform_probe(&dev) == -EPROBE_DEFER && probe_calls == 2);
    return 0;
}
""")

    def test_stop_mmio_errors_and_busy_status(self):
        stop = function(HERE / "aml_vdec_hw.c", "aml_stop_vdec_hw")
        run_c(COMMON + """
#include "reg_defines.h"
#define DOS_BUS 0
struct aml_vdec_hw { void *map[1]; int dec_irq; bool has_dbus_ctrl; };
static int transactions, fail_at, busy_reg, last_write, relax_count;
static int regmap_write(void *map, int reg, unsigned int value)
{
    if (++transactions == fail_at) return -EIO;
    last_write = reg;
    return 0;
}
static int regmap_read(void *map, int reg, unsigned int *value)
{
    if (++transactions == fail_at) return -EIO;
    *value = reg == VDEC_ASSIST_DBUS_DISABLE ? 0xffff : 0;
    if (reg == busy_reg)
        *value = reg == VDEC_ASSIST_DBUS_DISABLE ? 0 : 0xffff;
    return 0;
}
#define regmap_read_poll_timeout(map, reg, value, condition, delay, limit) \
    ({ int err = regmap_read(map, reg, &(value)); \
       if (!err && !(condition)) err = -ETIMEDOUT; err; })
static void synchronize_irq(int irq) {}
static void cpu_relax(void) { relax_count++; }
""" + stop + """
int main(void)
{
    struct aml_vdec_hw hw = { .has_dbus_ctrl = true };
    busy_reg = -1;
    assert(aml_stop_vdec_hw(&hw) == 0);
    assert(last_write == VDEC_ASSIST_DBUS_DISABLE && relax_count == 200);
    int total = transactions;
    for (int n = 1; n <= total; n++) {
        transactions = 0; fail_at = n;
        assert(aml_stop_vdec_hw(&hw) == -EIO);
        assert(transactions == n); /* No MMIO after a failed transaction. */
    }
    const int busy[] = { IMEM_DMA_CTRL, LMEM_DMA_CTRL, WRRSP_LMEM,
                         VDEC_ASSIST_DBUS_DISABLE };
    fail_at = 0;
    for (unsigned int n = 0; n < sizeof(busy) / sizeof(busy[0]); n++) {
        transactions = 0; busy_reg = busy[n];
        assert(aml_stop_vdec_hw(&hw) == -ETIMEDOUT);
    }
    busy_reg = -1; hw.has_dbus_ctrl = false;
    assert(aml_stop_vdec_hw(&hw) == 0);
    assert(last_write == DOS_SW_RESET0);
    return 0;
}
""")

    def test_finish_only_after_confirmed_stop_and_no_retry(self):
        stop_job = function(HERE / "aml_vdec.c", "aml_vdec_stop_job")
        run_c(COMMON + """
struct aml_vdec_hw { bool faulted; };
struct aml_codec_ops { void (*finish)(void *); };
struct dev { struct aml_vdec_hw *dec_hw; };
struct aml_vdec_ctx { struct dev *dev; bool dos_clk_en;
                      struct aml_codec_ops *codec_ops; };
static int stop_result, stops, finishes;
static int aml_stop_vdec_hw(struct aml_vdec_hw *hw)
{ stops++; return stop_result; }
static void aml_vdec_fault(struct aml_vdec_ctx *ctx)
{ ctx->dev->dec_hw->faulted = true; }
static void finish(void *ctx) { finishes++; }
""" + stop_job + """
int main(void)
{
    struct aml_vdec_hw hw = {0};
    struct dev dev = { &hw };
    struct aml_codec_ops ops = { finish };
    struct aml_vdec_ctx ctx = { &dev, true, &ops };
    stop_result = -ETIMEDOUT;
    aml_vdec_stop_job(&ctx);
    assert(hw.faulted && !finishes && stops == 1 && ctx.dos_clk_en);
    aml_vdec_stop_job(&ctx);
    assert(stops == 1);
    hw.faulted = false; stop_result = 0;
    aml_vdec_stop_job(&ctx);
    assert(stops == 2 && finishes == 1 && !ctx.dos_clk_en);
    aml_vdec_stop_job(&ctx);
    assert(stops == 2 && finishes == 1);
    return 0;
}
""")

    def test_vb2_close_keeps_faulted_plane_references(self):
        cleanup = function(HERE / "aml_vdec.c", "vb2_ops_vdec_buf_cleanup")
        run_c(COMMON + """
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
struct dma_buf { int puts; };
struct hw { bool faulted; };
struct dev { struct hw *dec_hw; };
struct aml_vdec_ctx { struct dev *dev; };
struct queue { struct aml_vdec_ctx *ctx; };
struct vb2_buffer { unsigned int num_planes; struct queue *vb2_queue; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct m2m { struct vb2_v4l2_buffer vb; };
struct aml_vdec_buffer { struct m2m m2m; struct dma_buf *pins[2]; };
#define to_vb2_v4l2_buffer(vb) container_of(vb, struct vb2_v4l2_buffer, vb2_buf)
#define vb2_get_drv_priv(q) ((q)->ctx)
static void dma_buf_put(struct dma_buf *buf) { buf->puts++; }
""" + cleanup + """
int main(void)
{
    struct hw hw = { .faulted = true };
    struct dev dev = { &hw };
    struct aml_vdec_ctx ctx = { &dev };
    struct queue q = { &ctx };
    struct dma_buf y = {0}, uv = {0};
    struct aml_vdec_buffer buf = {
        .m2m.vb.vb2_buf = { 2, &q }, .pins = { &y, &uv },
    };
    vb2_ops_vdec_buf_cleanup(&buf.m2m.vb.vb2_buf);
    assert(!y.puts && !uv.puts);
    hw.faulted = false;
    vb2_ops_vdec_buf_cleanup(&buf.m2m.vb.vb2_buf);
    assert(y.puts == 1 && uv.puts == 1);
    return 0;
}
""")

    def test_dos_lease_and_permanent_quarantine(self):
        source = HERE.parent / "t7-dos.c"
        code = "\n".join(function(source, name) for name in
                         ("meson_t7_dos_get", "meson_t7_dos_put",
                          "meson_t7_dos_poison"))
        run_c(COMMON + """
struct device { int unused; };
static int dos_lock, encoders, module_refs;
static bool decoder, poisoned;
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
#define dev_err(...) ((void)0)
#define THIS_MODULE 0
#define __module_get(module) (module_refs++)
""" + code + """
int main(void)
{
    assert(!meson_t7_dos_get(0, false));
    assert(!meson_t7_dos_get(0, false));
    assert(meson_t7_dos_get(0, true) == -EBUSY);
    meson_t7_dos_put(false);
    assert(meson_t7_dos_get(0, true) == -EBUSY);
    meson_t7_dos_put(false);
    assert(!meson_t7_dos_get(0, true));
    assert(meson_t7_dos_get(0, false) == -EBUSY);
    meson_t7_dos_put(true);
    meson_t7_dos_poison(); meson_t7_dos_poison();
    assert(module_refs == 1);
    assert(meson_t7_dos_get(0, false) == -EIO);
    assert(meson_t7_dos_get(0, true) == -EIO);
    return 0;
}
""")


if __name__ == "__main__":
    unittest.main()
