#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise V4L2 refresh controls and firmware row budgets for both codecs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
source = (root / 'wave5-vpu-enc.c').read_text()
helper = source[source.index('static void wave5_set_intra_refresh('):source.index('static void wave5_set_enc_picture_size(')]
controls = source[source.index('\tcase V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD:'):source.index('\tcase V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:')]
defines = '\n'.join(line for line in (root / 'wave5-vpuapi.h').read_text().splitlines()
                    if line.startswith(('#define REFRESH_MODE_', '#define REFRESH_MB_MODE_')))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <linux/v4l2-controls.h>
typedef uint32_t u32;
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
struct enc_wave_param {
    u32 intra_refresh_mode, intra_refresh_arg;
    u32 intra_mb_refresh_mode, intra_mb_refresh_arg;
};
struct vpu_instance { unsigned int intra_refresh_period; };
struct control { unsigned id, val; };
'''
control = ('static int set_control(struct vpu_instance *inst,unsigned id,unsigned value) {\n'
           'struct control storage={id,value}, *ctrl=&storage; switch(id) {\n'+controls+
           'default: return -EINVAL; } return 0; }\n')
checks = r'''
int main(void) {
    struct vpu_instance inst={0};
    struct enc_wave_param param={0};
    assert(!set_control(&inst,V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD,45));
    assert(set_control(&inst,V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE,
        V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_RANDOM)==-EINVAL);
    assert(inst.intra_refresh_period==45);
    assert(!set_control(&inst,V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE,
        V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC));
    wave5_set_intra_refresh(&param,2160,inst.intra_refresh_period,false);
    assert(param.intra_mb_refresh_mode==1 && param.intra_mb_refresh_arg==3);
    assert(!param.intra_refresh_mode && !param.intra_refresh_arg);
    wave5_set_intra_refresh(&param,2160,inst.intra_refresh_period,true);
    assert(param.intra_refresh_mode==1 && param.intra_refresh_arg==1);
    assert(!param.intra_mb_refresh_mode && !param.intra_mb_refresh_arg);
    for(unsigned height=1;height<=2160;height+=13) for(unsigned period=1;period<=4096;period+=17)
        for(unsigned hevc=0;hevc<2;hevc++) {
            wave5_set_intra_refresh(&param,height,period,hevc);
            unsigned rows=DIV_ROUND_UP(height,hevc?64:16);
            unsigned refresh=hevc?param.intra_refresh_arg:param.intra_mb_refresh_arg;
            assert(refresh>=1 && refresh<=rows && refresh*period>=rows);
        }
    assert(!set_control(&inst,V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD,0));
    wave5_set_intra_refresh(&param,1080,inst.intra_refresh_period,true);
    assert(!param.intra_refresh_mode && !param.intra_refresh_arg);
    assert(!param.intra_mb_refresh_mode && !param.intra_mb_refresh_arg);
    puts("Wave5 cyclic row refresh, codec transitions and partial rows passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-refresh-') as directory:
    code = Path(directory) / 'refresh.c'
    binary = Path(directory) / 'refresh'
    code.write_text(prefix + defines + '\n' + helper + control + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
