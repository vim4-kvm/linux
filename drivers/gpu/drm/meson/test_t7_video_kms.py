#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute video atomic validation and DMA transactions with real GEM layouts."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile


directory = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dtb', type=Path)
parser.add_argument('--vendor', type=Path)
args = parser.parse_args()
source = (directory / 'meson_t7_video_kms.c').read_text()
if args.dtb:
    node = '/display@ff006840'

    def prop(path, name, kind='x'):
        return subprocess.check_output(['fdtget', '-t', kind, str(args.dtb), path, name],
                                       text=True).split()

    names = prop(node, 'reg-names', 's')
    cells = [int(value, 16) for value in prop(node, 'reg')]
    regions = [(cells[i + 1], cells[i + 3]) for i in range(0, len(cells), 4)]
    capture = (directory.parents[3] / 'drivers/media/platform/amlogic/t7-hdmirx/t7-vdin-test.c').read_text()
    capture = capture.split('} regions[] = {')[1].split('};')[0]
    capture_regions = [(int(a, 16), int(b, 16)) for a, b in
                       re.findall(r'\{\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+)\s*}', capture)]
    assert capture_regions and len(names) == len(regions) == len(set(names))
    for i, (start, size) in enumerate(regions):
        for other, length in regions[i + 1:]:
            assert max(start, other) >= min(start + size, other + length)
    for start, size in regions:
        for a, b in capture_regions:
            assert max(start, a) >= min(start + size, a + b)
    for name, start, words in re.findall(r'{ "([\w-]+)", (0x[0-9a-f]+), (0x[0-9a-f]+|\d+) }', source):
        assert regions[names.index(name)] == (0xff000000 + int(start, 0) * 4,
                                             int(words, 0) * 4)
    assert prop(node, 'amlogic,video-planes') == []
    assert prop(node, 'amlogic,managed-osd-sram') == []
    domains = [int(value, 16) for value in prop(node, 'power-domains')]
    assert len(domains) == 30 and len(set(domains[::2])) == 1
    assert domains[-9::2] == [54, 55, 100, 86, 103]
    print(f'Compiled video DT: {len(regions)} disjoint capture/display banks and 15 exclusive domains passed')
handoff_test = (directory / 'test_t7_handoff.py').read_text()
prefix = re.search(r"prefix = r'''(.*?)'''", handoff_test, re.S)[1]
# Share the fake GEM/atomic ABI with the retirement test, executing real video code.
prefix = prefix[:prefix.index('struct meson_t7_video_kms {')] + '\n'
prefix += r'''
#include <string.h>
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define WARN_ON_ONCE(x) assert(!(x))
static bool ready=true;
static unsigned long vpu_rate=666000000;
static bool meson_t7_display_ready(struct drm_crtc *crtc) {return ready;}
static unsigned long meson_t7_display_clock_rate(struct drm_crtc *crtc) {return vpu_rate;}
static bool drm_rect_equals(const struct drm_rect *a,const struct drm_rect *b)
{return !memcmp(a,b,sizeof(*a));}
static struct drm_plane_state *previous[2];
static struct drm_plane_state *drm_atomic_get_old_plane_state(
    struct drm_atomic_commit *commit, struct drm_plane *plane)
{
    for (unsigned int i=0;i<2;i++) if (commit->planes[i]==plane && previous[i])
        return previous[i];
    return drm_atomic_get_new_plane_state(commit,plane);
}
'''
body = prefix
for filename in ('meson_t7_video.h', 'meson_t7_video_kms.h', 'meson_t7_layers.h',
                 'meson_t7_video.c', 'meson_t7_video_pps.c', 'meson_t7_fb.h'):
    text = (directory / filename).read_text()
    body += re.sub(r'^#include[^\n]*\n', '', text, flags=re.M) + '\n'
body += source[source.index('static const struct {'):source.index('struct meson_t7_video_kms *meson_t7_video_map')]
body += source[source.index('static int video_frame('):source.index('static void video_plane_update(')]
body += source[source.index('bool meson_t7_video_in_use('):source.index('int meson_t7_video_zpos_check(')]
body += source[source.index('static void video_write('):]
if args.vendor:
    vendor = (args.vendor / 'drivers/media/video_sink/video_hw.c').read_text()
    vendor = vendor[vendor.index('void vpp_blend_update_t7('):]
    ports = vendor[vendor.index('u32 port_val[4]'):vendor.index('set_value = vpp_misc_set;')]
    body += r'''
static u32 vendor_ports[0x6000];
static void vendor_write(u32 reg,u32 value) {vendor_ports[reg]=value;}
static struct {bool vd2_independ_blend_ctrl;
    struct {void (*rdma_wr)(u32,u32);} rdma_func[1];}
    vendor_device={.rdma_func={{vendor_write}}}, *cur_dev=&vendor_device;
static struct {bool pre_blend_en,post_blend_en;u32 layer_alpha;} vd_layer[3];
static struct {u32 cur_sel_port;} glayer_info[3];
#define VD1_BLEND_SRC_CTRL 0x1dfb
#define VD2_BLEND_SRC_CTRL 0x1dfc
#define VD3_BLEND_SRC_CTRL 0x1def
#define OSD2_BLEND_SRC_CTRL 0x1dfe
static void vendor_blend(u32 a,u32 b)
{
    u32 vpp_index=0,vpp_off=0;
    int i;
    memset(vd_layer,0,sizeof(vd_layer));
    vd_layer[0].post_blend_en=vd_layer[1].post_blend_en=true;
    vd_layer[0].layer_alpha=vd_layer[1].layer_alpha=256;
    glayer_info[0].cur_sel_port=a;glayer_info[1].cur_sel_port=b;
    glayer_info[2].cur_sel_port=2;
'''
    body += ports + '}\n#define CHECK_VENDOR_PORTS(a,b) do {vendor_blend(a,b);' \
        'const u32 r[]={0x1dfb,0x1dfc,0x1def,0x1dfe};' \
        'for (u32 n=0;n<4;n++) assert(hw[r[n]]==vendor_ports[r[n]]);} while(0)\n'
else:
    body += '#define CHECK_VENDOR_PORTS(a,b) ((void)0)\n'
body += r'''
static u32 hw[0x6000], tx[MESON_T7_VIDEO_KMS_PAIRS*2+2];
static void apply(int bytes) {
    assert(bytes>0 && bytes<=MESON_T7_VIDEO_KMS_PAIRS*8);
    assert(tx[MESON_T7_VIDEO_KMS_PAIRS*2]==0xa5a5a5a5);
    for (int i=0;i<bytes/4;i+=2) {
        bool claimed=false;
        for (unsigned int n=0;n<ARRAY_SIZE(video_banks);n++)
            claimed |= tx[i]>=video_banks[n].start &&
                tx[i]<video_banks[n].start+video_banks[n].words;
        assert(claimed);
        assert(tx[i]!=0x27a2 && tx[i]!=0x27a3); /* VDIN write arbitration. */
        hw[tx[i]]=tx[i+1];
    }
}
int main(void)
{
    struct meson_t7_video_kms video={};
    struct drm_crtc crtc={};
    struct drm_crtc_state running={.active=true,
        .adjusted_mode={1920,1080,74250,2200,1125}};
    const struct drm_format_info nv12={DRM_FORMAT_NV12,{1,2},2};
    const struct drm_format_info rgb={DRM_FORMAT_XRGB8888,{4},1};
    struct drm_gem_dma_object y={.dma_addr=0x20000000,.base.size=4096ULL*2160};
    struct drm_gem_dma_object uv={.dma_addr=0x24000000,.base.size=4096ULL*1080};
    struct drm_framebuffer fb={.format=&nv12,.width=3840,.height=2160,
        .pitches={4096,4096},.objects={&y,&uv}};
    struct drm_plane_state states[2]={
        {.fb=&fb,.crtc=&crtc,.visible=true,.src_w=3840U<<16,.src_h=2160U<<16,
         .crtc_w=1920,.crtc_h=1080,.normalized_zpos=0},
        {.fb=&fb,.crtc=&crtc,.visible=true,.src_w=3840U<<16,.src_h=2160U<<16,
         .crtc_w=1280,.crtc_h=720,.normalized_zpos=1}};
    struct drm_atomic_commit commit={.planes={&video.planes[0],&video.planes[1]},
        .states={&states[0],&states[1]},.new_crtc=&running};
    struct meson_t7_osd_layer osd[3]={{.enabled=true,.zpos=2},{.enabled=true,.zpos=3}};
    for (unsigned int i=0;i<ARRAY_SIZE(video_banks);i++)
        video.banks[i]=&hw[video_banks[i].start];
    hw[0x1a7a]=0x21;
    assert(!video_boot_check(&video));
    const u32 foreign[]={0x4800,0x4880,0x4900,0x4840,0x48c0,0x4940,
                         0x1d91,0x1d92,0x1a7a,0x1a8a};
    for (unsigned int i=0;i<ARRAY_SIZE(foreign);i++) {
        u32 r=foreign[i],saved=hw[r];
        hw[r]^=i>=3 && i<6 ? BIT(8) : BIT(0);
        assert(video_boot_check(&video)==-EBUSY);
        assert(hw[r]!=(saved)); /* Refusal cannot reset a foreign fetch. */
        hw[r]=saved;
    }
    assert(!video_plane_check(&video.planes[0],&commit));
    assert(!video_plane_check(&video.planes[1],&commit));
    running.adjusted_mode.clock=148500;
    assert(!video_plane_check(&video.planes[0],&commit)); /* 4K->1080p60 vskip. */
    memset(tx,0xa5,sizeof(tx));
    int skip_bytes=meson_t7_video_build(&video,&commit,false,false,osd,tx,
                                       MESON_T7_VIDEO_KMS_PAIRS);
    apply(skip_bytes);
    for(unsigned int layer=0;layer<2;layer++)
        for(unsigned int pat=0xc;pat<=0xf;pat++)
            assert(hw[0x4800+layer*0x80+pat]==8);
    assert(hw[0x1d08]==1U<<24 && hw[0x3938]==3U<<23);
    assert(hw[0x1d02]==1080 && hw[0x1a57]==(1080U<<16|3840));
    assert(hw[0x4804]==(2159U<<16)); /* Full allocation scope; MIF skips rows. */
    states[0].src_h-=2U<<16;
    assert(video_plane_check(&video.planes[0],&commit)==-EINVAL);
    states[0].src_h+=2U<<16;
    running.adjusted_mode=(struct drm_display_mode){3840,2160,594000,4400,2250};
    states[0].crtc_w=3840; states[0].crtc_h=2160;
    assert(!video_plane_check(&video.planes[0],&commit)); /* Native 4K60. */
    vpu_rate=250000000;
    assert(video_plane_check(&video.planes[0],&commit)==-EINVAL); /* PPS too slow. */
    vpu_rate=666000000;
    states[0].crtc_w=1920; states[0].crtc_h=1080;
    running.adjusted_mode=(struct drm_display_mode){1920,1080,74250,2200,1125};
    assert(!video_plane_check(&video.planes[0],&commit));
    struct drm_framebuffer *frames[2]={};
    /* An OSD-only commit has no state for either unattached video plane. */
    commit.states[0]=commit.states[1]=NULL;
    frames[0]=frames[1]=&fb;
    assert(!meson_t7_video_collect(&video,&commit,false,frames));
    assert(!frames[0] && !frames[1] && !meson_t7_video_in_use(&video,&commit));
    commit.states[0]=&states[0];
    assert(!meson_t7_video_collect(&video,&commit,false,frames));
    assert(frames[0]==&fb && !frames[1]);
    assert(!meson_t7_video_collect(&video,&commit,true,frames));
    assert(!frames[0] && !frames[1]);
    commit.states[1]=&states[1];
    assert(!meson_t7_video_collect(&video,&commit,false,frames));
    assert(frames[0]==&fb && frames[1]==&fb && meson_t7_video_in_use(&video,&commit));
    memset(tx,0xa5,sizeof(tx));
    hw[0x1d26]=0xa5a5a5a5; hw[0x1a83]=hw[0x1a84]=0xffffffff;
    int bytes=meson_t7_video_build(&video,&commit,false,false,osd,tx,
                                  MESON_T7_VIDEO_KMS_PAIRS);
    apply(bytes);
    for(unsigned int layer=0;layer<2;layer++)
        for(unsigned int pat=0xc;pat<=0xf;pat++)
            assert(!hw[0x4800+layer*0x80+pat]); /* 30Hz restores every row. */
    assert(!hw[0x4800] && !hw[0x4880]); /* Configuration cannot initiate DMA. */
    assert(hw[0x4820]==y.dma_addr>>4 && hw[0x4821]==uv.dma_addr>>4);
    assert(hw[0x48a0]==y.dma_addr>>4 && hw[0x48a1]==uv.dma_addr>>4);
    assert(hw[0x1d1c]==1919 && hw[0x1d1d]==1079);
    assert(hw[0x1d1e]==1279 && hw[0x1d1f]==719);
    assert(hw[0x1d1a]==3839 && hw[0x1d20]==3840 && hw[0x1d1b]==2303);
    assert((hw[0x1a83]&0x30)==0x10 && (hw[0x1a84]&0x30)==0x10);
    assert((hw[0x1d26]&0xe00)==(0xa5a5a5a5&0xe00));
    bytes=meson_t7_video_build(&video,&commit,false,true,osd,tx,
                              MESON_T7_VIDEO_KMS_PAIRS);
    apply(bytes);
    assert((hw[0x4800]&1) && (hw[0x4880]&1));
    assert((hw[0x1dfb]&0xf00)==0x100 && (hw[0x1dfc]&0xf00)==0x200);
    CHECK_VENDOR_PORTS(0,1);
    struct drm_plane_state old[2]={states[0],states[1]};
    previous[0]=&old[0]; previous[1]=&old[1];
    running.modeset=true;
    assert(!meson_t7_video_flip(&video,&commit,false,tx,6));
    running.modeset=false;
    bytes=meson_t7_video_flip(&video,&commit,false,tx,6);
    assert(bytes==48);
    for (int i=0;i<bytes/4;i+=2)
        assert((tx[i]>=0x4820 && tx[i]<=0x4822) ||
               (tx[i]>=0x48a0 && tx[i]<=0x48a2));
    apply(bytes);
    old[1].dst.x2--;
    assert(!meson_t7_video_flip(&video,&commit,false,tx,6));
    old[1].dst.x2++;
    assert(!meson_t7_video_flip(&video,&commit,true,tx,6));
    assert(meson_t7_video_flip(&video,&commit,false,tx,5)==-ENOSPC);
    states[1].normalized_zpos=4;
    bytes=meson_t7_video_build(&video,&commit,false,true,osd,tx,
                              MESON_T7_VIDEO_KMS_PAIRS);
    apply(bytes);
    assert((hw[0x1dfe]&0xf00)==0x200); /* VD2 above the whole OSD group. */
    CHECK_VENDOR_PORTS(0,3);
    states[0].normalized_zpos=5;
    assert(meson_t7_video_build(&video,&commit,false,true,osd,tx,
                              MESON_T7_VIDEO_KMS_PAIRS)==-EINVAL);
    states[0].normalized_zpos=0; states[1].normalized_zpos=2;
    osd[0].zpos=1; osd[1].zpos=3;
    assert(meson_t7_video_build(&video,&commit,false,true,osd,tx,
                              MESON_T7_VIDEO_KMS_PAIRS)==-EINVAL);
    osd[0].zpos=3; osd[1].zpos=4;
    assert(meson_t7_video_build(&video,&commit,false,false,osd,tx,
                              MESON_T7_VIDEO_KMS_PAIRS-1)==-ENOSPC);
    ready=false; assert(video_plane_check(&video.planes[0],&commit)==-EIO); ready=true;
    fb.offsets[1]=1;
    assert(video_plane_check(&video.planes[0],&commit)==-ERANGE); fb.offsets[1]=0;
    uv.base.size--;
    assert(video_plane_check(&video.planes[0],&commit)==-ERANGE); uv.base.size++;
    fb.format=&rgb;
    assert(video_plane_check(&video.planes[0],&commit)==-EINVAL); fb.format=&nv12;
    states[0].fb=states[1].fb=NULL; /* RMFB/detach leaves copied visibility true. */
    assert(!video_plane_check(&video.planes[0],&commit));
    assert(!video_plane_check(&video.planes[1],&commit));
    assert(!meson_t7_video_collect(&video,&commit,false,frames));
    assert(!frames[0] && !frames[1]);
    bytes=meson_t7_video_build(&video,&commit,true,false,osd,tx,
                              MESON_T7_VIDEO_KMS_PAIRS);
    apply(bytes);
    assert(!hw[0x4800] && !hw[0x4880]);
    assert(!(hw[0x1dfb]&0xf00) && !(hw[0x1dfc]&0xf00) && !(hw[0x1dfe]&0xf00));
}
'''
with tempfile.TemporaryDirectory() as work:
    root = Path(work)
    (root / 'video.c').write_text(body)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-fsanitize=undefined', str(root / 'video.c'), '-o', str(root / 'video')],
                   check=True)
    subprocess.run([str(root / 'video')], check=True)
print('T7 video KMS: two GEM objects, scaling, mixing, detach and bounded transactions passed')
