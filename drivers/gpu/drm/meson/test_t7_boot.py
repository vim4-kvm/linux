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
    u32 boot_format, boot_stride, boot_ctrl2;
};
static u32 readl(void *p) {return *(u32 *)p;}
'''
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
    puts("Boot takeover: linear contract, AFBC/unpack refusal and nine fetch/timing mismatches passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'boot.c').write_text(prefix + registers + defines + '\n' + function + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'boot.c'), '-o', str(path / 'boot')], check=True)
    subprocess.run([str(path / 'boot')], check=True)
