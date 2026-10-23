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
    def test_mdec_single_read_requires_all_prerequisites_and_verified_power_off(self):
        functions = "\n".join(function(HERE / "aml_vdec_hw.c", name) for name in (
            "t7_prepare_mdec", "aml_vdec_t7_mdec_test"))
        run_c(COMMON + """
#include "reg_defines.h"
#define DOS_BUS 0
#define dev_info(...) ((void)0)
struct aml_vdec_hw {
    bool has_dbus_ctrl, powered, power_test_passed, mdec_test_passed;
    u32 mdec_test_value;
    void *dev, *map[1];
};
static int fail_at, bad_at, calls, pic_reads, on_error, off_error, off_calls;
static const int registers[] = {
    DOS_SW_RESET0, DOS_MEM_PD_VDEC, DOS_GCLK_EN0, DOS_GCLK_EN0,
    GCLK_EN, GCLK_EN, MDEC_PIC_DC_CTRL
};
static int access_reg(int reg)
{
    assert(calls < 7 && registers[calls] == reg);
    calls++;
    if (reg == MDEC_PIC_DC_CTRL) pic_reads++;
    return fail_at == calls ? -EIO : 0;
}
static int regmap_read(void *map, int reg, u32 *value)
{
    int ret = access_reg(reg);
    *value = (reg == DOS_SW_RESET0 || reg == DOS_MEM_PD_VDEC) ? 0 : 0x3ff;
    if (calls == bad_at) *value ^= 1;
    return ret;
}
static int regmap_update_bits(void *map, int reg, u32 mask, u32 value)
{
    assert(reg == DOS_GCLK_EN0 && mask == 0x3ff && value == 0x3ff);
    return access_reg(reg);
}
static int regmap_write(void *map, int reg, u32 value)
{
    assert(reg == GCLK_EN && value == 0x3ff);
    return access_reg(reg);
}
static int t7_power_on(struct aml_vdec_hw *hw)
{ hw->powered = !on_error; return on_error; }
static int t7_power_off(struct aml_vdec_hw *hw)
{ hw->powered = false; off_calls++; return off_error; }
""" + functions + """
int main(void)
{
    struct aml_vdec_hw hw = { .has_dbus_ctrl = true };
    assert(aml_vdec_t7_mdec_test(&hw) == -EPERM && !calls && !off_calls);
    hw.power_test_passed = true;
    on_error = -EIO;
    assert(aml_vdec_t7_mdec_test(&hw) == -EIO && !calls && !off_calls);
    on_error = 0;
    for (int i = 1; i <= 7; i++) {
        calls = pic_reads = off_calls = 0; fail_at = i;
        assert(aml_vdec_t7_mdec_test(&hw) == -EIO);
        assert(calls == i && pic_reads == (i == 7) && off_calls == 1);
        assert(!hw.mdec_test_passed && !hw.powered);
    }
    fail_at = 0;
    const int checks[] = {1, 2, 4, 6};
    for (unsigned int i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        calls = pic_reads = off_calls = 0; bad_at = checks[i];
        assert(aml_vdec_t7_mdec_test(&hw) < 0);
        assert(!pic_reads && off_calls == 1 && !hw.mdec_test_passed);
    }
    bad_at = calls = pic_reads = off_calls = 0;
    off_error = -EIO;
    assert(aml_vdec_t7_mdec_test(&hw) == -EIO && !hw.mdec_test_passed);
    calls = pic_reads = off_calls = off_error = 0;
    assert(!aml_vdec_t7_mdec_test(&hw));
    assert(calls == 7 && pic_reads == 1 && off_calls == 1);
    assert(hw.mdec_test_passed && !hw.powered && hw.mdec_test_value == 0x3ff);
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
static bool allow_t7_power;
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
    assert(!aml_vdec_runtime_resume(&dev) && power_calls == 1);
    allow_t7_power = false;
    assert(aml_vdec_runtime_resume(&dev) == -EPERM && power_calls == 1);
    return 0;
}
""")

    def test_power_test_never_touches_dos_before_secure_domains_report_on(self):
        functions = "\n".join(function(HERE / "aml_vdec_hw.c", name) for name in (
            "t7_domain_state", "aml_vdec_t7_power_state", "t7_power_fault",
            "t7_power_off", "t7_power_on", "aml_vdec_t7_power_test"))
        run_c(COMMON + """
#include "reg_defines.h"
typedef int32_t s32;
#define EHOSTDOWN 112
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
    u32 power_test_reset;
    void *dev, *map[1];
    struct clk_bulk_data gates[2];
    struct pm *pm;
};
static int vdec_state, nic3_state, pm_error, off_error;
static int clocks, refs, mmio, poison, phase;
static void arm_smccc_smc(unsigned int cmd, unsigned int id, int a, int b,
                         int c, int d, int e, int f, struct arm_smccc_res *res)
{
    assert(cmd == 0x82000095);
    assert(id == 4 || id == 39);
    res->a0 = id == 4 ? vdec_state : nic3_state;
}
static int clk_set_rate(void *clk, int rate) { assert(rate == 800000000); return 0; }
static int clk_bulk_prepare_enable(int n, void *gates)
{ assert(phase == 0); phase = 1; clocks++; return 0; }
static void clk_bulk_disable_unprepare(int n, void *gates)
{ assert(clocks == 1); clocks--; phase = 3; }
static int pm_runtime_resume_and_get(void *dev)
{ assert(phase == 1); phase = 2; if (pm_error) return pm_error; refs++; return 0; }
static int pm_runtime_put_sync_suspend(void *dev)
{ assert(phase == 3); refs--; if (!off_error) vdec_state = 1; return off_error; }
static void __module_get(void *module) { poison++; }
static void get_device(void *dev) {}
static void meson_t7_dos_poison(void) {}
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
