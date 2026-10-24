#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual TX CSC writes, AVI range and EDID format admission together."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('meson_hdmitx21.c').read_text()


def function(start, end):
    return source[source.index(start):source.index(end)]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define HDMI_INFOFRAME_SIZE(x) 17
#define HDMI_COLORSPACE_RGB 0
#define HDMI_COLORSPACE_YUV444 2
#define HDMI_COLORIMETRY_ITU_709 2
#define HDMI_QUANTIZATION_RANGE_DEFAULT 0
#define HDMI_YCC_QUANTIZATION_RANGE_LIMITED 0
#define DRM_OUTPUT_COLOR_FORMAT_YCBCR444 1
struct meson_hdmitx21 { int unused; };
struct drm_connector { int unused; };
struct drm_display_mode { int hdisplay, vdisplay; };
struct drm_display_info { bool is_hdmi; unsigned int color_formats; };
struct hdmi_avi_infoframe {
    int colorspace, colorimetry, quantization_range, ycc_quantization_range;
};
static struct hdmi_avi_infoframe packet;
static u8 cor[4096];
static void cor_write(struct meson_hdmitx21 *tx, u32 reg, u8 value) { cor[reg]=value; }
static void cor_update(struct meson_hdmitx21 *tx, u32 reg, u8 mask, u8 value)
{ cor[reg]=(cor[reg]&~mask)|(value&mask); }
static void cor_pulse(struct meson_hdmitx21 *tx, u32 reg, u8 mask) { }
static void top_write(struct meson_hdmitx21 *tx, u32 reg, u32 value) { }
static int drm_hdmi_avi_infoframe_from_display_mode(struct hdmi_avi_infoframe *f,
    struct drm_connector *c, const struct drm_display_mode *m)
{ *f=(struct hdmi_avi_infoframe){99,99,99,99}; return 0; }
static ssize_t hdmi_avi_infoframe_pack(struct hdmi_avi_infoframe *f, u8 *buf, size_t n)
{ packet=*f; return 17; }
static void meson_hdmitx21_write_infoframe(struct meson_hdmitx21 *tx, u8 sel,
    const u8 *buf, ssize_t len) { assert(sel==0 && len==17); }
'''
program += '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
program += '\n' + function('static void meson_hdmitx21_write_avi',
                           'static int meson_hdmitx21_write_avi_infoframe')
program += function('static bool meson_hdmitx21_yuv_sink',
                    'static int meson_hdmitx21_bridge_attach')
program += r'''
int main(void)
{
    struct meson_hdmitx21 tx = {0};
    struct drm_connector connector = {0};
    struct drm_display_mode modes[] = {{1280,720},{1920,1080},{3840,2160}};
    /* RGB-only, 422-only, 420-only and DVI must not consume YUV444 bytes.
     * Arbitrary extra EDID flags cannot change that contract.
     */
    for (int hdmi=0; hdmi<2; hdmi++)
        for (int formats=0; formats<16; formats++) {
            struct drm_display_info info={hdmi,formats};
            assert(meson_hdmitx21_yuv_sink(&info)==(hdmi && (formats&2)));
        }
    for (int i=0; i<3; i++)
        for (int scramble=0; scramble<2; scramble++) {
            meson_hdmitx21_config(&tx,&modes[i],true,scramble);
            meson_hdmitx21_write_avi(&tx,&connector,&modes[i]);
            assert(cor[CSC0_CTRL]==0 && cor[CSC1_CTRL]==0);
            assert(cor[CSC0_CTRL2]==8 && cor[CSC1_CTRL2]==8);
            assert(cor[VP_INPUT_MASK]==0x2a);
            assert(cor[VP_OUTPUT_MAPPING]==0x40 && cor[VP_OUTPUT_MAPPING_HI]==4);
            assert(cor[VP_OUTPUT_MASK]==0);
            assert(packet.colorspace==HDMI_COLORSPACE_YUV444);
            assert(packet.colorimetry==HDMI_COLORIMETRY_ITU_709);
            assert(packet.quantization_range==HDMI_QUANTIZATION_RANGE_DEFAULT);
            assert(packet.ycc_quantization_range==HDMI_YCC_QUANTIZATION_RANGE_LIMITED);
        }
    puts("TX colour: 32 sink combinations; 720p/1080p/4K CSC and BT.709 limited AVI agree");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'color.c').write_text(program)
    subprocess.run(['cc', '-Werror', str(path / 'color.c'), '-o', str(path / 'color')],
                   check=True)
    subprocess.run([str(path / 'color')], check=True)
