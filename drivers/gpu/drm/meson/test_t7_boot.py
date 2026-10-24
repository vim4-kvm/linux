#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reject foreign AFBC and incompatible boot fetches before RDMA takeover."""
from pathlib import Path
import re
import subprocess
import tempfile


directory = Path(__file__).resolve().parent
source = (directory / 'meson_t7_handoff.c').read_text()
function = source[source.index('static int meson_t7_handoff_boot_check('):
                  source.index('static int meson_t7_handoff_probe(')]
tx_source = (directory / 'meson_hdmitx21.c').read_text()
tx_probe = tx_source[tx_source.index('static int meson_hdmitx21_boot_probe('):
                     tx_source.index('static int meson_hdmitx21_probe(')]
tx_guard = tx_probe[tx_probe.index('if (active != '):tx_probe.index('tx->bridge.of_node')]
defines = '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
registers = (directory / 'meson_registers.h').read_text()
registers = re.sub(r'^#include[^\n]*\n', '', registers, flags=re.M)
# Refusal must precede the first RDMA configuration write, not just DRM registration.
probe = source[source.index('static int meson_t7_handoff_probe('):]
assert probe.index('meson_t7_handoff_boot_check(display)') < probe.index('writel(')
# No remove path can release DMA after a failed drain: this is a boot-only device.
driver = source[source.index('static struct platform_driver meson_t7_handoff_platform_driver'):]
assert re.search(r'\.suppress_bind_attrs\s*=\s*true', driver)
kconfig = (directory / 'Kconfig').read_text().split('config DRM_MESON_T7_HANDOFF')[1]
assert re.match(r'\s*bool\s', kconfig)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX << (l)) & (UINT32_MAX >> (31-(h))))
#define drm_info(...) ((void)0)
struct meson_t7_handoff {
    void *osd, *venc;
    bool native_modeset, managed_sram;
    u32 boot_format, boot_stride, boot_ctrl2;
};
static u32 readl(void *p) {return *(u32 *)p;}
#define dev_err_probe(dev, error, ...) ((void)(dev), (error))
'''
tx_function = ('static int tx_boot(u32 active, bool enc2) { void *dev=NULL;(void)enc2;\n' +
               tx_guard + 'return 0; }\n')
checks = r'''
int main(void)
{
    u32 osd[32]={0}, venc[0x608/4]={0}, saved[32];
    struct meson_t7_handoff display={.osd=osd,.venc=venc};
    osd[VIU_OSD1_CTRL_STAT-VIU_OSD1_CTRL_STAT]=5;
    osd[VIU_OSD1_BLK0_CFG_W0-VIU_OSD1_CTRL_STAT]=0x8700;
    osd[VIU_OSD1_BLK1_CFG_W4-VIU_OSD1_CTRL_STAT]=HANDOFF_BOOT_ADDR>>4;
    osd[VIU_OSD1_BLK2_CFG_W4-VIU_OSD1_CTRL_STAT]=HANDOFF_PITCH>>4;
    osd[VIU_OSD1_BLK0_CFG_W1-VIU_OSD1_CTRL_STAT]=(HANDOFF_WIDTH-1)<<16;
    osd[VIU_OSD1_BLK0_CFG_W2-VIU_OSD1_CTRL_STAT]=(HANDOFF_HEIGHT-1)<<16;
    venc[ENCP_VIDEO_EN-VENC_INTCTRL]=1;
    venc[ENCP_VIDEO_MAX_PXCNT-VENC_INTCTRL]=2199;
    venc[ENCP_VIDEO_MAX_LNCNT-VENC_INTCTRL]=1124;
    memcpy(saved,osd,sizeof(osd));
    assert(!meson_t7_handoff_boot_check(&display));
    assert(display.boot_format==0x8700 && display.boot_stride==HANDOFF_PITCH>>4);
    assert(!memcmp(saved,osd,sizeof(osd)));
    osd[VIU_OSD1_BLK0_CFG_W0-VIU_OSD1_CTRL_STAT]|=BIT(30);
    assert(meson_t7_handoff_boot_check(&display)==-EBUSY);
    memcpy(osd,saved,sizeof(osd));
    osd[VIU_OSD1_MALI_UNPACK_CTRL-VIU_OSD1_CTRL_STAT]=BIT(31);
    assert(meson_t7_handoff_boot_check(&display)==-EBUSY);
    memcpy(osd,saved,sizeof(osd));
    const unsigned int fetch[] = {VIU_OSD1_CTRL_STAT,VIU_OSD1_BLK0_CFG_W0,
        VIU_OSD1_BLK1_CFG_W4,VIU_OSD1_BLK2_CFG_W4,VIU_OSD1_BLK0_CFG_W1,
        VIU_OSD1_BLK0_CFG_W2};
    for (unsigned int i=0;i<sizeof(fetch)/sizeof(fetch[0]);i++) {
        osd[fetch[i]-VIU_OSD1_CTRL_STAT]^=1;
        assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
        memcpy(osd,saved,sizeof(osd));
    }
    const unsigned int timing[]={ENCP_VIDEO_EN,ENCP_VIDEO_MAX_PXCNT,ENCP_VIDEO_MAX_LNCNT};
    for (unsigned int i=0;i<sizeof(timing)/sizeof(timing[0]);i++) {
        venc[timing[i]-VENC_INTCTRL]^=1;
        assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
        venc[timing[i]-VENC_INTCTRL]^=1;
    }
    assert(!meson_t7_handoff_boot_check(&display));
    /* The no-EDID timing is safe only with full native SRAM ownership. */
    venc[ENCP_VIDEO_MAX_PXCNT-VENC_INTCTRL]=1649;
    venc[ENCP_VIDEO_MAX_LNCNT-VENC_INTCTRL]=749;
    assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
    display.native_modeset=true;
    assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
    display.native_modeset=false;display.managed_sram=true;
    assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
    display.native_modeset=true;
    assert(!meson_t7_handoff_boot_check(&display));
    /* The exception must not bypass any of the nine fetch/timing guards. */
    for (unsigned int i=0;i<sizeof(fetch)/sizeof(fetch[0]);i++) {
        osd[fetch[i]-VIU_OSD1_CTRL_STAT]^=1;
        assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
        memcpy(osd,saved,sizeof(osd));
    }
    for (unsigned int i=0;i<sizeof(timing)/sizeof(timing[0]);i++) {
        venc[timing[i]-VENC_INTCTRL]^=1;
        assert(meson_t7_handoff_boot_check(&display)==-ENODEV);
        venc[timing[i]-VENC_INTCTRL]^=1;
    }
    osd[VIU_OSD1_BLK0_CFG_W0-VIU_OSD1_CTRL_STAT]|=BIT(30);
    assert(meson_t7_handoff_boot_check(&display)==-EBUSY);
    memcpy(osd,saved,sizeof(osd));
    osd[VIU_OSD1_MALI_UNPACK_CTRL-VIU_OSD1_CTRL_STAT]=BIT(31);
    assert(meson_t7_handoff_boot_check(&display)==-EBUSY);
    assert(!tx_boot(1080U<<16|1920,false));
    assert(tx_boot(720U<<16|1280,false)==-ENODEV);
    assert(!tx_boot(720U<<16|1280,true));
    const u32 invalid_active[]={0,480U<<16|640,720U<<16|1920,
        1080U<<16|1280,2160U<<16|3840};
    for (unsigned int i=0;i<sizeof(invalid_active)/sizeof(invalid_active[0]);i++) {
        assert(tx_boot(invalid_active[i],true)==-ENODEV);
        assert(tx_boot(invalid_active[i],false)==-ENODEV);
    }
    puts("Boot takeover: native 720p fallback and unchanged fetch/AFBC/legacy guards passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'boot.c').write_text(prefix + registers + defines + '\n' + function + tx_function + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'boot.c'), '-o', str(path / 'boot')], check=True)
    subprocess.run([str(path / 'boot')], check=True)
