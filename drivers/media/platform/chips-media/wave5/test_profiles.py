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
struct vpu_instance { struct vpu_device *dev; unsigned std, bit_depth;
    struct { u32 profile, constraint_set1_flag; } enc_param; };
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
    assert(inst.bit_depth==8);
    assert(wave5_enc_sps_profile(&inst,inst.enc_param.profile)==3);
    assert(!set_profile(&inst,V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10));
    assert(inst.bit_depth==10);
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
