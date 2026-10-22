#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check visible encoder geometry against the actual block crop calculation."""
from pathlib import Path
import subprocess
import tempfile

hardware = Path(__file__).with_name('wave5-hw.c').read_text()
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
        int width=modes[i][0], height=modes[i][1];
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
    }
    puts("Wave5 AVC visible geometry, even crop units and rotated padding passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-geometry-') as directory:
    source = Path(directory) / 'geometry.c'
    executable = Path(directory) / 'geometry'
    source.write_text(prefix + crop + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
