#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the actual firmware-command profile translation and control transitions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
hardware = (root / 'wave5-hw.c').read_text()
translation = hardware[hardware.index('static u32 wave5_enc_sps_profile('):
                       hardware.index('int wave5_vpu_enc_init_seq(')]
controls = (root / 'wave5-vpu-enc.c').read_text()
profile_control = controls[controls.index('\tcase V4L2_CID_MPEG_VIDEO_H264_PROFILE:'):
                           controls.index('\tcase V4L2_CID_MPEG_VIDEO_H264_LEVEL:')]
defines = '\n'.join(line for line in (root / 'wave5-vpuapi.h').read_text().splitlines()
                    if line.startswith('#define H264_PROFILE_'))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <linux/v4l2-controls.h>
typedef uint32_t u32;
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
struct vpu_device { bool legacy_firmware; };
struct vpu_instance { struct vpu_device *dev; unsigned std;
    struct { u32 profile, constraint_set1_flag, internal_bit_depth; } enc_param; };
struct control { unsigned id, val; };
'''
checks = r'''
int main(void)
{
    struct vpu_device dev = {.legacy_firmware=true};
    struct vpu_instance inst = {.dev=&dev,.std=W_AVC_ENC};
    /* A High request must issue legacy High, not the High10 command that
     * produced profile_idc=110 with 8-bit pixels in the hardware regression. */
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_HIGH));
    assert(inst.enc_param.internal_bit_depth==8);
    assert(wave5_enc_sps_profile(&inst,inst.enc_param.profile)==3);
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10));
    assert(inst.enc_param.internal_bit_depth==10);
    assert(wave5_enc_sps_profile(&inst,inst.enc_param.profile)==4);
    for (unsigned p=1;p<=7;p++) {
        dev.legacy_firmware=false;
        assert(wave5_enc_sps_profile(&inst,p)==p);
        dev.legacy_firmware=true; inst.std=W_HEVC_ENC;
        assert(wave5_enc_sps_profile(&inst,p)==p);
        inst.std=W_AVC_ENC;
    }
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE));
    assert(inst.enc_param.constraint_set1_flag==1);
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_HIGH));
    assert(inst.enc_param.constraint_set1_flag==0);
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE));
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE));
    assert(inst.enc_param.constraint_set1_flag==0);
    assert(wave5_enc_sps_profile(&inst,inst.enc_param.profile)==1);
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_MAIN));
    assert(wave5_enc_sps_profile(&inst,inst.enc_param.profile)==2);
    puts("Wave5 legacy AVC profiles and constraint transitions passed");
}
'''
function = ('static int set_profile(struct vpu_instance *inst, unsigned value) {\n'
            '__typeof__(inst->enc_param) *param=&inst->enc_param;\n'
            'struct control storage={.id=V4L2_CID_MPEG_VIDEO_H264_PROFILE,.val=value};\n'
            'struct control *ctrl=&storage; switch(ctrl->id) {\n' + profile_control +
            'default: return -EINVAL; } return 0; }\n')
with tempfile.TemporaryDirectory(prefix='wave5-profile-') as directory:
    source = Path(directory) / 'profiles.c'
    source.write_text(prefix + defines + '\n' + translation + function + checks)
    binary = Path(directory) / 'profiles'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Compile the complete setter too: separate extracted profile switches cannot
# catch the shared-storage bug when both codec control sets are configured.
api = (root / 'wave5-vpuapi.h').read_text()
param = api[api.index('struct enc_wave_param {'):]
param = param[:param.index('\n};') + 3]
setter = controls[controls.index('static int wave5_set_qp_limits('):
                  controls.index('static const struct v4l2_ctrl_ops')]
protocol = '\n'.join(line for line in api.splitlines()
                     if line.startswith(('#define HEVC_PROFILE_', '#define H264_PROFILE_')))
framework = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/v4l2-controls.h>
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define DIV_ROUND_UP_ULL(a,b) (((a)+(b)-1)/(b))
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define MIN_VBV_BUFFER_SIZE 10
#define MAX_VBV_BUFFER_SIZE 3000
#define VPU_INST_STATE_NONE 0
#define dev_dbg(...) ((void)0)
#ifndef V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE
#define V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE 0x00990b00
#define V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC 0
#endif
#ifndef V4L2_CID_MPEG_VIDEO_BACKGROUND_DETECTION
#define V4L2_CID_MPEG_VIDEO_BACKGROUND_DETECTION (V4L2_CID_CODEC_BASE + 238)
#endif
struct vpu_instance {
    struct enc_wave_param enc_param, hevc_param;
    unsigned state, encode_aud, mirror_direction, rot_angle, vbv_buf_size;
    unsigned rc_mode, bit_rate, intra_refresh_period, rc_enable, force_keyframe;
    unsigned h264_cpb_size, std, b_frames;
};
struct enc_open_param { unsigned vbv_buffer_size; };
struct v4l2_ctrl { unsigned id; int val; struct vpu_instance *owner; struct v4l2_ctrl **cluster; };
static struct vpu_instance *wave5_ctrl_to_vpu_inst(struct v4l2_ctrl *c) { return c->owner; }
static void atomic_set(unsigned *value, int setting) { *value=setting; }
'''
behavior = r'''
static int set(struct vpu_instance *inst,unsigned id,int value)
{ struct v4l2_ctrl ctrl={.id=id,.val=value,.owner=inst}; return wave5_vpu_enc_s_ctrl(&ctrl); }
int main(void)
{
    struct vpu_instance a={0}, b={0};
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_PROFILE,V4L2_MPEG_VIDEO_H264_PROFILE_HIGH));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_LEVEL,V4L2_MPEG_VIDEO_H264_LEVEL_5_1));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN));
    assert(!a.hevc_param.en_still_picture);
    assert(a.enc_param.profile==H264_PROFILE_HP && a.enc_param.level==51);
    assert(a.hevc_param.profile==HEVC_PROFILE_MAIN);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_TIER,V4L2_MPEG_VIDEO_HEVC_TIER_HIGH));
    assert(a.hevc_param.tier && !a.enc_param.tier);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,21));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,29));
    assert(a.hevc_param.fixed_qp_p==21 && a.enc_param.fixed_qp_p==29);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_MAX_NUM_MERGE_MV_MINUS1,0));
    assert(a.hevc_param.max_num_merge==1);
    struct v4l2_ctrl storage[8]={0}, *limits[8];
    for(unsigned i=0;i<8;i++) { limits[i]=&storage[i]; storage[i].val=i%2?51:0; }
    struct v4l2_ctrl master={.cluster=limits};
    storage[0].val=8; storage[4].val=19; storage[7].val=43;
    assert(!wave5_set_qp_limits(&a.hevc_param,&master));
    assert(a.hevc_param.min_qp_i==8 && a.hevc_param.min_qp_p==19);
    assert(a.hevc_param.max_qp_b==43);
    storage[1].val=18;
    assert(wave5_set_qp_limits(&a.hevc_param,&master)==-EINVAL);
    assert(a.hevc_param.max_qp_b==43 && a.hevc_param.min_qp_p==19);
    struct enc_open_param open={0};
    a.std=W_AVC_ENC; a.h264_cpb_size=1000; a.bit_rate=8192000;
    assert(!wave5_set_vbv_size(&open,&a) && open.vbv_buffer_size==1000);
    a.std=W_HEVC_ENC; a.vbv_buf_size=3001;
    assert(wave5_set_vbv_size(&open,&a)==-ERANGE);
    a.vbv_buf_size=1;
    assert(wave5_set_vbv_size(&open,&a)==-ERANGE);
    a.bit_rate=0;
    assert(wave5_set_vbv_size(&open,&a)==-EINVAL);
    a.vbv_buf_size=0; a.rc_mode=V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
    assert(!wave5_set_vbv_size(&open,&a) && open.vbv_buffer_size==3000);
    assert(!set(&a,V4L2_CID_HFLIP,1)); assert(!set(&a,V4L2_CID_VFLIP,1));
    assert(a.mirror_direction==3);
    assert(!set(&a,V4L2_CID_HFLIP,0)); assert(a.mirror_direction==1);
    assert(!set(&a,V4L2_CID_VFLIP,0)); assert(a.mirror_direction==0);
    a.state=1;
    assert(set(&a,V4L2_CID_MPEG_VIDEO_BITRATE,1000000)==-EBUSY);
    assert(a.bit_rate==0);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,0)); assert(a.force_keyframe);
    assert(!set(&b,V4L2_CID_MPEG_VIDEO_BITRATE,2000000));
    assert(a.bit_rate==0 && b.bit_rate==2000000);
    puts("Wave5 codec control isolation, busy setters, flip reversal and two instances passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-controls-') as directory:
    source = Path(directory) / 'controls.c'
    binary = Path(directory) / 'controls'
    # Basic types precede the real parameter structure; mock framework follows.
    split = framework.index('struct vpu_instance {')
    vbv = controls[controls.index('static int wave5_set_vbv_size('):
                   controls.index('static int wave5_set_enc_openparam(')]
    source.write_text(framework[:split] + protocol + '\n' + param +
                      framework[split:] + setter + vbv + behavior)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
