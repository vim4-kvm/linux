#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check visible encoder geometry against the actual block crop calculation."""
from pathlib import Path
import subprocess
import tempfile

hardware = Path(__file__).with_name('wave5-hw.c').read_text()
encoder = Path(__file__).with_name('wave5-vpu-enc.c').read_text()
picture = encoder[encoder.index('static void wave5_set_enc_picture_size('):
                  encoder.index('static int wave5_set_enc_openparam(')]
crop = hardware[hardware.index('static void wave5_set_enc_crop_info('):
                hardware.index('static u32 wave5_enc_sps_profile(')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define ALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define WARN(condition, ...) assert(!(condition))
struct enc_wave_param {
    int conf_win_top, conf_win_bot, conf_win_left, conf_win_right;
};
struct enc_open_param { unsigned pic_width, pic_height; struct enc_wave_param wave_param; };
struct vpu_instance { int std; struct { int left, top; unsigned width, height; } conf_win; };
'''
checks = r'''
int main(void)
{
    const int modes[][2] = {
        {640,480}, {720,400}, {800,600}, {1024,768}, {1280,720},
        {1280,1024}, {1366,768}, {1440,900}, {1600,900}, {1680,1050},
        {1920,1080}, {1920,1200}, {2560,1440}, {3840,2160}
    };
    for (unsigned i=0; i<sizeof(modes)/sizeof(modes[0]); i++) {
        unsigned width=modes[i][0], height=modes[i][1];
        struct enc_wave_param p={0};
        wave5_set_enc_crop_info(W_AVC_ENC,&p,0,width,height);
        assert(ALIGN(width,16)-p.conf_win_left-p.conf_win_right==width);
        assert(ALIGN(height,16)-p.conf_win_top-p.conf_win_bot==height);
        assert(!(p.conf_win_right%2) && !(p.conf_win_bot%2));
        /* Rotation swaps axes, including the non-block-aligned crop. */
        p=(struct enc_wave_param){0};
        wave5_set_enc_crop_info(W_AVC_ENC,&p,3,width,height);
        assert(ALIGN(height,16)-p.conf_win_left-p.conf_win_right==height);
        assert(ALIGN(width,16)-p.conf_win_top-p.conf_win_bot==width);
        struct vpu_instance inst={.std=W_HEVC_ENC,.conf_win={0,0,width,height}};
        struct enc_open_param op={0};
        wave5_set_enc_picture_size(&op,&inst);
        assert(op.pic_width==ALIGN(width,8) && op.pic_height==ALIGN(height,8));
        /* This is the value written by SRC_SIZE and CONF_WIN, not a software
         * header rewrite. The firmware must emit the corresponding SPS. */
        wave5_set_enc_crop_info(W_HEVC_ENC,&op.wave_param,0,op.pic_width,op.pic_height);
        assert(op.pic_width-op.wave_param.conf_win_right==width);
        assert(op.pic_height-op.wave_param.conf_win_bot==height);
        for (unsigned rotation=0; rotation<4; rotation++) {
            op=(struct enc_open_param){0};
            inst.conf_win.left=2; inst.conf_win.top=4;
            wave5_set_enc_picture_size(&op,&inst);
            wave5_set_enc_crop_info(W_HEVC_ENC,&op.wave_param,rotation?1|(rotation<<1):0,
                                   op.pic_width,op.pic_height);
            unsigned coded_w=rotation?ALIGN(op.pic_width,32):op.pic_width;
            unsigned coded_h=rotation?ALIGN(op.pic_height,32):op.pic_height;
            if (rotation&1) { unsigned swap=coded_w; coded_w=coded_h; coded_h=swap; }
            assert(coded_w-op.wave_param.conf_win_left-op.wave_param.conf_win_right==
                   (rotation&1?height:width));
            assert(coded_h-op.wave_param.conf_win_top-op.wave_param.conf_win_bot==
                   (rotation&1?width:height));
        }
    }
    puts("Wave5 AVC visible geometry, even crop units and rotated padding passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-geometry-') as directory:
    source = Path(directory) / 'geometry.c'
    executable = Path(directory) / 'geometry'
    source.write_text(prefix + crop + picture + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
