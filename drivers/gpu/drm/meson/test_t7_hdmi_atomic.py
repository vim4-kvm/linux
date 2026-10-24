#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute HDMI atomic admission and packet bank ownership without hardware."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here / 'meson_hdmitx21.c').read_text()
clock = (here / 'meson_t7_modeset.c').read_text()
header = (here / 'meson_t7_modeset.h').read_text()


def function(text, name):
    match = re.search(r'^(?:static )?[\w \t*\n]+\b' + name + r'\([^;]*?\)\s*\{', text, re.M)
    depth, end = 1, match.end()
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define min(a,b) ((a)<(b)?(a):(b))
#define DIV_ROUND_CLOSEST(a,b) (((a)+(b)/2)/(b))
#define IS_ENABLED(x) 1
#define DRM_OUTPUT_COLOR_FORMAT_RGB444 0
#define DRM_OUTPUT_COLOR_FORMAT_YCBCR444 1
#define DRM_OUTPUT_COLOR_FORMAT_YCBCR420 3
#define DRM_MODE_COLORIMETRY_DEFAULT 0
#define DRM_MODE_COLORIMETRY_BT709_YCC 2
#define DRM_MODE_COLORIMETRY_BT2020_YCC 10
#define DRM_HDMI_BROADCAST_RGB_LIMITED 2
#define HDMI_QUANTIZATION_RANGE_FULL 2
#define HDMI_QUANTIZATION_RANGE_DEFAULT 0
#define HDMI_YCC_QUANTIZATION_RANGE_LIMITED 0
#define HDMI_COLORIMETRY_ITU_709 2
#define HDMI_EOTF_BT_2100_HLG 3
struct drm_bridge {int unused;};
struct drm_bridge_state {int unused;};
struct drm_plane {int unused;};
struct drm_plane_state {void *crtc;struct drm_framebuffer *fb;};
struct drm_framebuffer {struct {bool is_yuv;} *format;};
struct drm_property_blob {const void *data;unsigned int length;};
struct hdr_output_metadata {unsigned int metadata_type;
    struct {unsigned int metadata_type,eotf;} hdmi_metadata_type1;};
struct drm_display_info {bool is_hdmi;
    struct {struct {bool supported;struct {bool supported;} scrambling;} scdc;} hdmi;
    struct {struct {unsigned int eotf;} hdmi_type1;} hdr_sink_metadata;};
struct drm_connector {struct drm_display_info display_info;};
struct hdmi_avi_infoframe {unsigned int quantization_range,ycc_quantization_range,colorimetry;};
struct drm_connector_state {struct drm_connector *connector;unsigned int colorspace;
    struct drm_property_blob *hdr_output_metadata;
    struct {unsigned int output_format,output_bpc,broadcast_rgb;
        unsigned long long tmds_char_rate;bool is_limited_range;
        struct {struct {struct {struct hdmi_avi_infoframe avi;} data;} avi;} infoframes;} hdmi;};
struct drm_crtc_state {void *state,*crtc;bool mode_changed;
    struct {int clock,hdisplay,htotal;} adjusted_mode;};
struct meson_hdmitx21 {bool atomic_color;int packet_lock;};
static struct drm_connector_state old;
static struct drm_plane test_plane;
static struct drm_plane_state test_plane_state;
static bool affected;
static struct meson_hdmitx21 tx={.atomic_color=true};
static struct meson_hdmitx21 *bridge_to_hdmitx(struct drm_bridge *b) {return &tx;}
static int drm_atomic_add_affected_planes(void *s,void *c) {affected=true;return 0;}
#define for_each_new_plane_in_state(s,p,ps,i) \
    for (i=0,p=&test_plane,ps=&test_plane_state; i<1; i++)
static struct drm_connector_state *drm_atomic_get_old_connector_state(void *s,void *c)
{return &old;}
static bool drm_connector_atomic_hdr_metadata_equal(const struct drm_connector_state *a,
    const struct drm_connector_state *b) {return a->hdr_output_metadata==b->hdr_output_metadata;}
static u8 packets[8][31],enable[8];
static unsigned int bank,locked;
static void mutex_lock(int *lock) {assert(!locked);locked=1;}
static void mutex_unlock(int *lock) {assert(locked);locked=0;}
static void cor_write(struct meson_hdmitx21 *t,u32 reg,u8 value) {
    assert(locked);
    if(reg==0x6bf) {assert(value<8);bank=value;}
    else if(reg==0x6df) enable[bank]=value;
    else {assert(reg>=0x6c0 && reg<0x6df);packets[bank][reg-0x6c0]=value;}
}
'''
program += '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
program += '\n' + header[header.index('struct meson_t7_clock {'):
                  header.index('int meson_t7_link_clock_calculate')]
program += function(clock, 'meson_t7_link_clock_calculate')
for name in ['meson_hdmitx21_bridge_atomic_check', 'meson_hdmitx21_write_infoframe',
             'meson_hdmitx21_write_hdr_infoframe', 'meson_hdmitx21_clear_hdr_infoframe']:
    program += '\n' + function(source, name)
program += r'''
int main(void) {
    struct drm_connector connector={.display_info={.is_hdmi=true}};
    struct drm_connector_state conn={.connector=&connector,
        .hdmi={.output_bpc=8,.tmds_char_rate=148500000}};
    struct drm_crtc_state crtc={.crtc=&connector,.adjusted_mode={148500,1920,2200}};
    assert(!meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn) && affected);
    assert(conn.hdmi.infoframes.avi.data.avi.quantization_range==HDMI_QUANTIZATION_RANGE_FULL);
    conn.hdmi.broadcast_rgb=DRM_HDMI_BROADCAST_RGB_LIMITED;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    conn.hdmi.broadcast_rgb=0;
    conn.hdmi.output_format=DRM_OUTPUT_COLOR_FORMAT_YCBCR444;
    assert(!meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn));
    assert(conn.hdmi.infoframes.avi.data.avi.quantization_range==0);
    struct hdr_output_metadata hdr={.hdmi_metadata_type1.eotf=2};
    struct drm_property_blob blob={&hdr,sizeof(hdr)};
    conn.hdr_output_metadata=&blob;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    conn.colorspace=DRM_MODE_COLORIMETRY_BT2020_YCC;
    conn.hdmi.output_bpc=10;conn.hdmi.tmds_char_rate=185625000;
    connector.display_info.hdr_sink_metadata.hdmi_type1.eotf=BIT(2)|BIT(3);
    assert(!meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn) && crtc.mode_changed);
    for(unsigned int e=0;e<8;e++) {
        hdr.hdmi_metadata_type1.eotf=e;
        assert((meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==0)==(e==2||e==3));
    }
    hdr.hdmi_metadata_type1.eotf=2;blob.length--;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    blob.length++;hdr.metadata_type=1;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    hdr.metadata_type=0;
    struct {bool is_yuv;} format={true};
    struct drm_framebuffer fb={.format=(void *)&format};
    test_plane_state.crtc=crtc.crtc;test_plane_state.fb=&fb;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    test_plane_state.fb=NULL; /* Detached video must not block an HDR OSD commit. */
    assert(!meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn));
    crtc.adjusted_mode.clock=594000;crtc.adjusted_mode.hdisplay=3840;crtc.adjusted_mode.htotal=4400;
    conn.hdmi.output_format=DRM_OUTPUT_COLOR_FORMAT_YCBCR420;
    conn.hdmi.tmds_char_rate=371250000;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    connector.display_info.hdmi.scdc.supported=true;
    connector.display_info.hdmi.scdc.scrambling.supported=true;
    assert(!meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn));
    crtc.adjusted_mode.htotal++;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-EINVAL);
    crtc.adjusted_mode.htotal--;conn.hdmi.output_format=DRM_OUTPUT_COLOR_FORMAT_RGB444;
    assert(meson_hdmitx21_bridge_atomic_check(NULL,NULL,&crtc,&conn)==-ERANGE);
    u8 payload[30];for(unsigned int i=0;i<30;i++) payload[i]=i+1;
    enable[2]=0xe0;packets[2][0]=0x84;
    assert(!meson_hdmitx21_write_hdr_infoframe(NULL,payload,30));
    assert(enable[6]==0xe0 && packets[6][29]==30 && !packets[6][30]);
    assert(!meson_hdmitx21_clear_hdr_infoframe(NULL) && !enable[6]);
    assert(enable[2]==0xe0 && packets[2][0]==0x84 && !locked);
    puts("HDMI atomic: HDR/EOTF/depth/range, mixed-gamut refusal, 420/SCDC, RMFB and packet clearing passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'atomic.c').write_text(program)
    subprocess.run(['cc', '-Werror', str(path / 'atomic.c'), '-o', str(path / 'atomic')], check=True)
    subprocess.run([str(path / 'atomic')], check=True)
