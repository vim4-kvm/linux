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

# LTR is one slot on Wave521: the two picture bits are independent requests.
ltr_write = hardware[hardware.index('\tvpu_write_reg(inst->dev, W5_CMD_ENC_PIC_LONGTERM_PIC,'):]
ltr_write = ltr_write[:ltr_write.index(';') + 1]
ltr_test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define W5_CMD_ENC_PIC_LONGTERM_PIC 0
struct vpu_instance { int dev; };
struct enc_param { bool mark_long_term, use_long_term; };
static unsigned command;
static void vpu_write_reg(int dev,int reg,unsigned value)
{ (void)dev; (void)reg; command=value; }
'''
with tempfile.TemporaryDirectory(prefix='wave5-ltr-') as directory:
    code = Path(directory) / 'ltr.c'
    binary = Path(directory) / 'ltr'
    code.write_text(ltr_test + 'static void pack(struct vpu_instance *inst, '
                    'struct enc_param *option) {\n' + ltr_write + '\n}\n' + r'''
int main(void) {
    struct vpu_instance inst={0};
    for(unsigned mask=0;mask<4;mask++) {
        struct enc_param option={.mark_long_term=mask&1,.use_long_term=mask&2};
        pack(&inst,&option); assert(command==mask);
    }
    puts("Wave5 independent LTR mark/use picture bits passed");
}
''')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute SET_PARAM packing and completion ownership with scripted firmware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).parent
api = (root / 'wave5-vpuapi.h').read_text()
param = api[api.index('struct enc_wave_param {'):]
param = param[:param.index('\n};') + 3]
defines = '\n'.join(line for line in api.splitlines()
                    if line.startswith(('#define ENC_CHANGE_', '#define ENC_AVC_',
                                        '#define ENC_HEVC_')))
hardware = (root / 'wave5-hw.c').read_text()
defines += '\n' + '\n'.join(line for line in hardware.splitlines()
                             if line.startswith(('#define ENC_AVC_', '#define ENC_HEVC_')))
change = hardware[hardware.index('int wave5_vpu_enc_change('):
                  hardware.index('int wave5_vpu_enc_get_seq_info(')]
source = (root / 'wave5-vpuapi.c').read_text()
wrapper = source[source.index('int wave5_vpu_enc_change_param('):
                 source.index('static int wave5_check_enc_param(')]
mock = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "wave5-regdefine.h"
typedef uint32_t u32;
typedef int32_t s32;
#define BIT(n) (1U<<(n))
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define OPT_CHANGE_PARAM 0x10
#define VPU_ENC_TIMEOUT 5000
#define RECON_IDX_FLAG_CHANGE_PARAM (-4)
'''
framework = r'''
struct enc_open_param { struct enc_wave_param wave_param; u32 bit_rate, vbv_buffer_size; };
struct vpu_device { int hw_lock; };
struct codec { struct { struct enc_open_param open_param; } enc_info; };
struct vpu_instance { struct vpu_device *dev; struct codec *codec_info; unsigned std; int irq_done; };
struct enc_output_info { int recon_frame_index; };
static u32 regs[0x1000/4], writes[0x1000/4];
static int ack, completion, report_error, report_index, queries, waits, isolated;
static int mutex_lock_interruptible(int *lock) { assert(!*lock); *lock=1; return 0; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static void reinit_completion(int *done) { *done=0; }
static void vpu_write_reg(struct vpu_device *dev, unsigned reg, u32 value)
{ assert(dev->hw_lock && reg<sizeof(regs)); regs[reg/4]=value; writes[reg/4]++; }
static int send_firmware_command(struct vpu_instance *inst, int cmd, bool wait, void *a, void *b)
{ (void)a; (void)b; assert(inst->dev->hw_lock && cmd==W5_ENC_SET_PARAM && wait); return ack; }
static int wave5_vpu_wait_interrupt(struct vpu_instance *inst, unsigned timeout)
{ assert(!inst->dev->hw_lock && timeout==5000); waits++; return completion; }
static int wave5_vpu_enc_get_output_info(struct vpu_instance *inst, struct enc_output_info *out)
{ assert(!inst->dev->hw_lock); queries++; out->recon_frame_index=report_index; return report_error; }
static void wave5_vpu_isolate(struct vpu_device *dev, const char *reason, int command)
{ (void)dev; (void)reason; (void)command; isolated++; }
'''
checks = r'''
int main(void)
{
    struct vpu_device dev={0}; struct codec codec={0};
    struct vpu_instance inst={.dev=&dev,.codec_info=&codec,.std=W_AVC_ENC};
    struct enc_open_param p={.bit_rate=4000000,.vbv_buffer_size=1000};
    p.wave_param=(struct enc_wave_param){.intra_qp=27,.intra_period=30,.avc_idr_period=60,
        .min_qp_i=8,.max_qp_i=40,.min_qp_p=9,.max_qp_p=41,.min_qp_b=10,.max_qp_b=42,
        .beta_offset_div2=-2,.tc_offset_div2=-3,.chroma_cb_qp_offset=-4,
        .chroma_cr_qp_offset=5,.transform8x8_enable=1,.entropy_coding_mode=1};
    report_index=-4;
    assert(!wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_BITRATE|ENC_CHANGE_RC));
    assert(codec.enc_info.open_param.bit_rate==4000000 && queries==1 && waits==1);
    assert(regs[W5_CMD_ENC_SEQ_RC_TARGET_RATE/4]==4000000);
    assert(regs[W5_CMD_ENC_SEQ_RC_PARAM/4]==(1000U<<20));
    assert(!writes[W5_CMD_ENC_SEQ_SPS_PARAM/4] && !writes[W5_CMD_ENC_SEQ_SRC_SIZE/4]);
    assert(!wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_PPS|ENC_CHANGE_INTRA|
                                      ENC_CHANGE_QP|ENC_CHANGE_INTER_QP));
    assert((regs[W5_CMD_ENC_SEQ_PPS_PARAM/4]>>6&15)==14);
    assert((regs[W5_CMD_ENC_SEQ_PPS_PARAM/4]>>10&15)==13);
    assert(regs[W5_CMD_ENC_SEQ_INTRA_PARAM/4]==(27U|30U<<6|60U<<17));
    assert(regs[W5_CMD_ENC_SEQ_RC_INTER_MIN_MAX_QP/4]==(9U|41U<<6|10U<<12|42U<<18));
    inst.std=W_HEVC_ENC;
    assert(!wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_INTRA));
    assert(regs[W5_CMD_ENC_SEQ_INTRA_PARAM/4]==(27U<<3|30U<<16));
    int old_queries=queries, old_waits=waits;
    p.bit_rate=2000000;
    assert(!wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_PICTURE));
    assert(queries==old_queries && waits==old_waits);
    p.bit_rate=1000000; ack=-EINVAL;
    assert(wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_BITRATE)==-EINVAL);
    assert(codec.enc_info.open_param.bit_rate==2000000 && queries==old_queries);
    ack=0; completion=-ETIMEDOUT;
    assert(wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_BITRATE)==-ETIMEDOUT);
    assert(codec.enc_info.open_param.bit_rate==2000000 && queries==old_queries);
    completion=0; report_error=-EIO;
    assert(wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_BITRATE)==-EIO);
    assert(codec.enc_info.open_param.bit_rate==2000000);
    report_error=0; report_index=0;
    assert(wave5_vpu_enc_change_param(&inst,&p,ENC_CHANGE_BITRATE)==-EIO && isolated==1);
    assert(codec.enc_info.open_param.bit_rate==2000000 && !dev.hw_lock);
    puts("Wave5 dynamic packing, completion ownership and rejection/timeout rollback passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-changes-') as directory:
    code = Path(directory) / 'changes.c'
    binary = Path(directory) / 'changes'
    code.write_text(mock + defines + '\n' + param + framework + change + wrapper + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root), str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


# Check the actual picture-command packing, including simultaneous IDR and QP.
write = hardware[hardware.index('\tvpu_write_reg(inst->dev, W5_CMD_ENC_PIC_PIC_PARAM,'):]
write = write[:write.index(';') + 1]
packing = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define W5_CMD_ENC_PIC_PIC_PARAM 0
struct vpu_instance { int dev; };
struct enc_param { bool force_keyframe, force_qp; u32 qp_i, qp_p, qp_b; };
static u32 command;
static void vpu_write_reg(int dev,unsigned reg,u32 value)
{ (void)dev; (void)reg; command=value; }
'''
packing_checks = r'''
int main(void)
{
    struct vpu_instance inst={0};
    struct enc_param option={0};
    for(unsigned key=0;key<2;key++) for(unsigned enable=0;enable<2;enable++)
        for(unsigned qp=0;qp<=51;qp++) {
            option=(struct enc_param){key,enable,qp,51-qp,qp/2};
            pack(&inst,&option);
            assert(((command>>20)&1)==key && ((command>>21)&7)==(key?3:0));
            assert(((command>>1)&1)==enable && !(command&1));
            assert(((command>>2)&63)==(enable?qp:0));
            assert(((command>>8)&63)==(enable?51-qp:0));
            assert(((command>>14)&63)==(enable?qp/2:0));
        }
    puts("Wave5 fixed QP and simultaneous IDR command packing passed (208 cases)");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-picture-') as directory:
    code = Path(directory) / 'picture.c'
    binary = Path(directory) / 'picture'
    wrapper = ('static void pack(struct vpu_instance *inst,struct enc_param *option) {\n' +
               write + '\n}\n')
    code.write_text(packing + wrapper + packing_checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Compile the complete setter too: separate extracted profile switches cannot
# catch the shared-storage bug when both codec control sets are configured.
api = (root / 'wave5-vpuapi.h').read_text()
param = api[api.index('struct enc_wave_param {'):]
param = param[:param.index('\n};') + 3]
setter = controls[controls.index('static int wave5_set_qp_limits('):
                  controls.index('static const struct v4l2_ctrl_ops')]
gop_pic = api[api.index('struct custom_gop_pic_param {'):]
param += '\n' + gop_pic[:gop_pic.index('\n};') + 3]
protocol = '\n'.join(line for line in api.splitlines()
                     if line.startswith(('#define HEVC_PROFILE_', '#define H264_PROFILE_',
                                         '#define ENC_CHANGE_')))
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
#define DIV_ROUND_CLOSEST(a,b) (((a)+(b)/2)/(b))
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define MIN_VBV_BUFFER_SIZE 10
#define MAX_VBV_BUFFER_SIZE 3000
#define BIT(n) (1ULL<<(n))
#define VPU_INST_STATE_NONE 0
#define VPU_INST_STATE_PIC_RUN 3
#define MAX_GOP_NUM 8
#define PRESET_IDX_CUSTOM_GOP 0
#define PRESET_IDX_IBPBP 4
#define PRESET_IDX_IPP_SINGLE 9
#define PIC_TYPE_P 1
#define PIC_TYPE_B 2
#define __ffs(value) __builtin_ctz(value)
#define ARRAY_SIZE(array) (sizeof(array)/sizeof((array)[0]))
#define dev_dbg(...) ((void)0)
#ifndef V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE
#define V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE 0x00990b00
#define V4L2_CID_MPEG_VIDEO_INTRA_REFRESH_PERIOD_TYPE_CYCLIC 0
#endif
#ifndef V4L2_CID_MPEG_VIDEO_BACKGROUND_DETECTION
#define V4L2_CID_MPEG_VIDEO_BACKGROUND_DETECTION (V4L2_CID_CODEC_BASE + 238)
#endif
struct vpu_instance;
struct v4l2_ctrl { unsigned id; int val; struct vpu_instance *owner; struct v4l2_ctrl **cluster;
    int minimum, maximum, step; uint64_t skip; };
struct v4l2_ctrl_handler { int error; unsigned count; struct vpu_instance *owner;
    struct v4l2_ctrl controls[128]; };
struct vpu_device { bool legacy_firmware; };
struct vpu_instance {
    struct enc_wave_param enc_param, hevc_param;
    unsigned state, encode_aud, mirror_direction, rot_angle, vbv_buf_size;
    unsigned rc_mode, bit_rate, intra_refresh_period, rc_enable, force_keyframe;
    unsigned h264_cpb_size, std, b_frames, quality, enc_change_mask, ltr_count, ltr_pending;
    struct v4l2_ctrl_handler v4l2_ctrl_hdl;
    struct v4l2_ctrl *avc_qp_limits[8], *hevc_qp_limits[8];
    struct vpu_device *dev;
};
struct enc_open_param { unsigned vbv_buffer_size, custom_gop_size;
    struct enc_wave_param wave_param; struct custom_gop_pic_param custom_gop[8]; };
static struct vpu_instance *wave5_ctrl_to_vpu_inst(struct v4l2_ctrl *c) { return c->owner; }
static void atomic_set(unsigned *value, int setting) { *value=setting; }
static void atomic_or(unsigned mask,unsigned *value) { *value|=mask; }
static void atomic_andnot(unsigned mask,unsigned *value) { *value&=~mask; }
static int wave5_vpu_enc_ctrl_ops;
static void v4l2_ctrl_handler_init(struct v4l2_ctrl_handler *hdl,unsigned count)
{ assert(count<=128 && !hdl->count); }
static struct v4l2_ctrl *v4l2_ctrl_new_std(struct v4l2_ctrl_handler *hdl,void *ops,
        unsigned id,int minimum,int maximum,int step,int value)
{
    (void)ops;
    if(id==V4L2_CID_MPEG_VIDEO_USE_LTR_FRAMES) assert(step==0);
    else if(id!=V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) assert(step>0);
    assert(hdl->count<128 && minimum<=value && value<=maximum);
    struct v4l2_ctrl *ctrl=&hdl->controls[hdl->count++];
    *ctrl=(struct v4l2_ctrl){.id=id,.val=value,.owner=hdl->owner,
        .minimum=minimum,.maximum=maximum,.step=step};
    return ctrl;
}
static struct v4l2_ctrl *v4l2_ctrl_new_std_menu(struct v4l2_ctrl_handler *hdl,void *ops,
        unsigned id,int maximum,uint64_t skip,int value)
{
    assert(!(skip&BIT(value)));
    struct v4l2_ctrl *ctrl=v4l2_ctrl_new_std(hdl,ops,id,0,maximum,1,value);
    ctrl->skip=skip; return ctrl;
}
static struct v4l2_ctrl *v4l2_ctrl_find(struct v4l2_ctrl_handler *hdl,unsigned id)
{ for(unsigned i=0;i<hdl->count;i++) if(hdl->controls[i].id==id) return &hdl->controls[i]; return NULL; }
static void v4l2_ctrl_cluster(unsigned count,struct v4l2_ctrl **limits)
{ for(unsigned i=0;i<count;i++) limits[i]->cluster=limits; }
'''
behavior = r'''
static int set(struct vpu_instance *inst,unsigned id,int value)
{
    struct v4l2_ctrl *ctrl=v4l2_ctrl_find(&inst->v4l2_ctrl_hdl,id);
    assert(ctrl);
    if(value<ctrl->minimum || value>ctrl->maximum) return -ERANGE;
    if(ctrl->skip && (ctrl->skip&BIT(value))) return -EINVAL;
    int old=ctrl->val; ctrl->val=value;
    int ret=wave5_vpu_enc_s_ctrl(ctrl->cluster?ctrl->cluster[0]:ctrl);
    if(ret) ctrl->val=old;
    return ret;
}
int main(void)
{
    struct vpu_device dev={.legacy_firmware=true};
    struct vpu_instance a={0}, b={0};
    a.dev=b.dev=&dev;
    a.v4l2_ctrl_hdl.owner=&a; b.v4l2_ctrl_hdl.owner=&b;
    create_controls(&a); create_controls(&b);
    for(unsigned i=0;i<a.v4l2_ctrl_hdl.count;i++) {
        struct v4l2_ctrl *ctrl=&a.v4l2_ctrl_hdl.controls[i];
        if(!ctrl->cluster || ctrl->cluster[0]==ctrl)
            assert(!wave5_vpu_enc_s_ctrl(ctrl));
    }
    a.force_keyframe=0;
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,52)==-ERANGE);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_H264_MIN_QP,-1)==-ERANGE);
    assert(a.enc_param.min_qp_i==0 && a.hevc_param.min_qp_i==0);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_MAX_QP,0));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_MAX_QP,51));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,0));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,51));
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEVC_MAX_NUM_MERGE_MV_MINUS1,2)==-ERANGE);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_B_FRAMES,8)==-ERANGE);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2)==-ERANGE);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,101)==-ERANGE);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEADER_MODE,V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE)==-EINVAL);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_MIN_QP,20));
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,19)==-ERANGE);
    assert(a.hevc_param.min_qp_p==20 && a.hevc_param.max_qp_p==51);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_MIN_QP,8));
    /* The framework can clamp minimum - 1 to zero before s_ctrl. */
    assert(set(&a,V4L2_CID_MPEG_VIDEO_H264_MAX_QP,0)==-ERANGE);
    assert(a.enc_param.min_qp_i==8 && a.enc_param.max_qp_i==51);
    struct enc_wave_param quality={.intra_qp=29,.fixed_qp_p=30,.fixed_qp_b=31};
    wave5_set_constant_quality(&quality,&a);
    assert(quality.intra_qp==29 && quality.fixed_qp_p==30 && quality.fixed_qp_b==31);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_BITRATE_MODE,V4L2_MPEG_VIDEO_BITRATE_MODE_CQ));
    unsigned previous=52;
    for(unsigned q=1;q<=100;q++) {
        assert(!set(&a,V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,q));
        wave5_set_constant_quality(&quality,&a);
        assert(quality.intra_qp<=previous && quality.intra_qp<=51);
        assert(quality.fixed_qp_p==quality.intra_qp && quality.fixed_qp_b==quality.intra_qp);
        if(q==1) assert(quality.intra_qp==51);
        previous=quality.intra_qp;
    }
    assert(quality.intra_qp==0);
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
    assert(wave5_set_qp_limits(&a.hevc_param,&master)==-ERANGE);
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
    a.state=VPU_INST_STATE_PIC_RUN; a.std=W_AVC_ENC; a.h264_cpb_size=0;
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_BITRATE,4000000));
    assert(a.bit_rate==4000000 && a.enc_change_mask==(ENC_CHANGE_BITRATE|ENC_CHANGE_RC));
    a.enc_change_mask=0;
    assert(set(&a,V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE,2000)==-ERANGE);
    assert(!a.h264_cpb_size && !a.enc_change_mask);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE,400));
    assert(a.enc_change_mask==ENC_CHANGE_RC);
    a.enc_change_mask=0;
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,25));
    assert(!a.enc_change_mask && a.hevc_param.fixed_qp_p==25);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,25));
    assert(a.enc_change_mask==ENC_CHANGE_PICTURE);
    a.enc_change_mask=0;
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_H264_MIN_QP,10));
    assert(a.enc_change_mask==(ENC_CHANGE_QP|ENC_CHANGE_INTER_QP));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_GOP_SIZE,30));
    assert(a.enc_change_mask&ENC_CHANGE_INTRA);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,0)==-EBUSY);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_H264_PROFILE,V4L2_MPEG_VIDEO_H264_PROFILE_MAIN)==-EBUSY);
    assert(!b.enc_change_mask && b.bit_rate==2000000);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_LTR_COUNT,1)==-EBUSY);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_FRAME_LTR_INDEX,0)==-EINVAL);
    a.ltr_count=1;
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_FRAME_LTR_INDEX,0));
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_USE_LTR_FRAMES,1));
    assert(a.ltr_pending==3 && !b.ltr_pending);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_USE_LTR_FRAMES,0));
    assert(a.ltr_pending==1);
    a.ltr_pending=0;
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_FRAME_LTR_INDEX,0));
    assert(a.ltr_pending==1);
    a.state=VPU_INST_STATE_NONE; a.std=W_HEVC_ENC; a.ltr_count=0;
    for(unsigned codec=1;codec<=2;codec++) {
        a.std=codec;
        struct enc_wave_param *input=codec==W_HEVC_ENC?&a.hevc_param:&a.enc_param;
        input->hier_enabled=false;
        for(unsigned count=2;count<8;count++) {
            a.b_frames=count;
            open=(struct enc_open_param){.wave_param={.fixed_qp_p=26,.fixed_qp_b=28,
                .max_qp_p=51,.max_qp_b=51}};
            assert(!wave5_set_enc_gop(&open,&a) && open.custom_gop_size==count+1);
            assert(open.custom_gop[0].poc_offset==count+1);
            for(unsigned i=1;i<=count;i++) assert(open.custom_gop[i].poc_offset==i);
        }
        for(unsigned layers=1;layers<=4;layers++) {
            input->hier_enabled=true; input->hier_qp_enable=true;
            input->hier_layers=layers;
            for(unsigned t=0;t<4;t++) input->hier_qp[t]=24+t;
            a.b_frames=(1U<<(layers-1))-1;
            open=(struct enc_open_param){.wave_param={.max_qp_p=51,.max_qp_b=51}};
            assert(!wave5_set_enc_gop(&open,&a));
            unsigned seen=1, size=a.b_frames+1;
            int temporal[9]={0};
            for(unsigned i=0;i<size;i++) {
                struct custom_gop_pic_param *pic=&open.custom_gop[i];
                unsigned poc=pic->poc_offset, left=pic->ref_poc_l0, right=pic->ref_poc_l1;
                assert(poc>0 && poc<=size && !(seen&BIT(poc)));
                assert(seen&BIT(left)); assert(temporal[left]<=pic->temporal_id);
                if(pic->pic_type==PIC_TYPE_B) {
                    assert(left<poc && right>poc && (seen&BIT(right)));
                    assert(temporal[right]<=pic->temporal_id);
                }
                assert(pic->pic_qp==24+(unsigned)pic->temporal_id);
                seen|=BIT(poc); temporal[poc]=pic->temporal_id;
            }
            assert(seen==BIT(size+1)-1);
            open.wave_param.intra_period=size+1;
            if(size>1) assert(wave5_set_enc_gop(&open,&a)==-EINVAL);
            open.wave_param.intra_period=size*4;
            assert(!wave5_set_enc_gop(&open,&a));
        }
        input->hier_layers=0;
        assert(wave5_set_enc_gop(&open,&a)==-EINVAL);
        input->hier_p=true; a.b_frames=0; a.ltr_count=0;
        for(unsigned layers=1;layers<=4;layers++) {
            input->hier_layers=layers;
            open=(struct enc_open_param){.wave_param={.max_qp_p=51,.max_qp_b=51}};
            assert(!wave5_set_enc_gop(&open,&a));
            for(unsigned i=0;i<open.custom_gop_size;i++) {
                struct custom_gop_pic_param *pic=&open.custom_gop[i];
                assert(pic->pic_type==PIC_TYPE_P && pic->poc_offset==i+1);
                assert(pic->ref_poc_l0<pic->poc_offset);
                if(pic->ref_poc_l0) assert(open.custom_gop[pic->ref_poc_l0-1].temporal_id
                                         <pic->temporal_id);
                assert(pic->pic_qp==24+(unsigned)pic->temporal_id);
            }
        }
        a.b_frames=1;
        assert(wave5_set_enc_gop(&open,&a)==-EINVAL);
    }
    a.std=W_HEVC_ENC; a.b_frames=0; a.state=VPU_INST_STATE_PIC_RUN;
    assert(set(&a,V4L2_CID_MPEG_VIDEO_GOP_SIZE,30)==-EINVAL);
    assert(!set(&a,V4L2_CID_MPEG_VIDEO_GOP_SIZE,32));
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,30)==-EBUSY);
    assert(set(&a,V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,20)==-EBUSY);
    unsigned old_min=a.hevc_param.min_qp_p;
    assert(set(&a,V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,28)==-ERANGE);
    assert(a.hevc_param.min_qp_p==old_min && a.hevc_param.hier_qp[0]==24);
    a.ltr_count=1;
    assert(wave5_set_enc_gop(&open,&a)==-EINVAL);
    a.hevc_param.hier_enabled=false; a.hevc_param.en_still_picture=true;
    assert(wave5_set_enc_gop(&open,&a)==-EINVAL);
    a.hevc_param.en_still_picture=false; a.b_frames=1;
    assert(wave5_set_enc_gop(&open,&a)==-EINVAL);
    a.b_frames=0;
    assert(!wave5_set_enc_gop(&open,&a));
    puts("Wave5 28 multi-B/hierarchical GOP graphs, boundaries and LTR combinations passed");
    puts("Wave5 codec control isolation, busy setters, flip reversal and two instances passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-controls-') as directory:
    source = Path(directory) / 'controls.c'
    binary = Path(directory) / 'controls'
    # Basic types precede the real parameter structure; mock framework follows.
    split = framework.index('struct vpu_instance;')
    vbv = controls[controls.index('static int wave5_set_vbv_size('):
                   controls.index('static int wave5_set_enc_openparam(')]
    cluster = controls[controls.index('static void wave5_create_qp_limits('):
                       controls.index('static int wave5_vpu_open_enc(')]
    registration = controls[controls.index('\tv4l2_ctrl_handler_init('):
                            controls.index('\tif (v4l2_ctrl_hdl->error)')]
    create = ('static void create_controls(struct vpu_instance *inst) {\n'
              'int ret;\n'
              'struct v4l2_ctrl_handler *v4l2_ctrl_hdl=&inst->v4l2_ctrl_hdl;\n'
              'struct vpu_device *dev=inst->dev;\n' + registration + '}\n')
    source.write_text(framework[:split] + protocol + '\n' + param +
                      framework[split:] + setter + vbv + cluster + create + behavior)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Exercise the real CUSTOM_GOP command, including both firmware ABIs and failure.
custom = hardware[hardware.index('\t/* CUSTOM_GOP precedes COMMON,'):]
custom = custom[:custom.index('\n\t/*\n\t * OPT_COMMON:')]
gop_command = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include "wave5-regdefine.h"
typedef uint32_t u32;
typedef int32_t s32;
#define BIT(n) (1U<<(n))
#define PRESET_IDX_CUSTOM_GOP 0
#define OPT_CUSTOM_GOP 1
#define MAX_GOP_NUM 8
'''
gop_mock = r'''
struct enc_wave_param { unsigned gop_preset_idx; };
struct enc_open_param { struct enc_wave_param wave_param; unsigned custom_gop_size;
    struct custom_gop_pic_param custom_gop[8]; };
struct vpu_device { bool legacy_firmware; };
struct vpu_instance { struct vpu_device *dev; };
static unsigned registers[0x1000/4], count, commands;
static int fail;
static void vpu_write_reg(struct vpu_device *dev,unsigned reg,unsigned value)
{ (void)dev; assert(reg<sizeof(registers)); registers[reg/4]=value; count++; }
static int send_firmware_command(struct vpu_instance *inst,int cmd,bool wait,void *a,void *b)
{
    (void)inst; (void)a; (void)b;
    assert(wait && cmd==W5_ENC_SET_PARAM && count==10);
    assert(registers[W5_CMD_ENC_SEQ_SET_PARAM_OPTION/4]==OPT_CUSTOM_GOP);
    commands++; return fail;
}
static int program(struct vpu_instance *inst,struct enc_open_param *p_open_param)
{
    struct enc_wave_param *p_param=&p_open_param->wave_param;
    u32 reg_val; unsigned i; int ret;
'''
gop_checks = r'''
int main(void)
{
    struct vpu_device dev={0}; struct vpu_instance inst={.dev=&dev};
    struct enc_open_param open={.custom_gop_size=4,.custom_gop={
        {.pic_type=1,.poc_offset=4,.pic_qp=24},
        {.pic_type=2,.poc_offset=2,.pic_qp=25,.ref_poc_l1=4,.temporal_id=1},
        {.pic_type=2,.poc_offset=1,.pic_qp=26,.ref_poc_l1=2,.temporal_id=2},
        {.pic_type=2,.poc_offset=3,.pic_qp=26,.ref_poc_l0=2,.ref_poc_l1=4,.temporal_id=2}}};
    for(unsigned legacy=0;legacy<2;legacy++) {
        dev.legacy_firmware=legacy; count=0;
        assert(!program(&inst,&open) && count==10);
        unsigned word=registers[(W5_CMD_ENC_CUSTOM_GOP_PIC_PARAM_0+12)/4];
        assert((word&3)==2 && (word>>2&15)==3 && (word>>6&63)==26);
        assert((word>>12&3)==legacy && (word>>14&31)==2 && (word>>19&31)==4);
        assert((word>>24&7)==2);
        for(unsigned i=4;i<8;i++) assert(!registers[(W5_CMD_ENC_CUSTOM_GOP_PIC_PARAM_0+i*4)/4]);
    }
    count=0; fail=-ETIMEDOUT;
    assert(program(&inst,&open)==-ETIMEDOUT && commands==3);
    open.wave_param.gop_preset_idx=9; count=0;
    assert(!program(&inst,&open) && !count && commands==3);
    puts("Wave5 custom GOP firmware ABI, unused slots and timeout boundary passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-gop-') as directory:
    code = Path(directory) / 'gop.c'
    binary = Path(directory) / 'gop'
    code.write_text(gop_command + gop_pic[:gop_pic.index('\n};') + 3] +
                    gop_mock + custom + '\nreturn 0;\n}\n' + gop_checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# A rejected deferred change must wake both queues without returning owned DMA.
apply_change = controls[controls.index('static int wave5_apply_enc_changes(struct vpu_instance *inst)\n{'):
                        controls.index('static int initialize_sequence(')]
apply_mock = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define VPU_INST_STATE_STOP 4
struct queue { unsigned errors, owned; };
struct v4l2_m2m_ctx { struct queue src, dst; };
struct enc_open_param { unsigned value; };
struct vpu_instance {
    struct { struct v4l2_m2m_ctx *m2m_ctx; } v4l2_fh;
    int *avc_qp_limits[1];
    unsigned enc_change_mask, state, requested, active;
};
static int build_error, command_error;
static unsigned calls, last_mask;
static int *locked;
static void v4l2_ctrl_lock(int *ctrl) { assert(!locked); locked=ctrl; }
static void v4l2_ctrl_unlock(int *ctrl) { assert(locked==ctrl); locked=NULL; }
static int wave5_set_enc_openparam(struct enc_open_param *param,struct vpu_instance *inst)
{ assert(locked==inst->avc_qp_limits[0]); param->value=inst->requested; return build_error; }
static int wave5_vpu_enc_change_param(struct vpu_instance *inst,
        const struct enc_open_param *param,unsigned mask)
{
    assert(locked==inst->avc_qp_limits[0]); calls++; last_mask=mask;
    if(!command_error) inst->active=param->value;
    return command_error;
}
static struct queue *v4l2_m2m_get_src_vq(struct v4l2_m2m_ctx *ctx) { return &ctx->src; }
static struct queue *v4l2_m2m_get_dst_vq(struct v4l2_m2m_ctx *ctx) { return &ctx->dst; }
static void vb2_queue_error(struct queue *queue) { queue->errors++; }
static void switch_state(struct vpu_instance *inst,unsigned state) { inst->state=state; }
'''
apply_checks = r'''
int main(void)
{
    int control[2];
    struct v4l2_m2m_ctx queues[2]={{{0,3},{0,2}},{{0,1},{0,1}}};
    struct vpu_instance inst[2]={
        {.v4l2_fh.m2m_ctx=&queues[0],.avc_qp_limits={&control[0]},.state=3},
        {.v4l2_fh.m2m_ctx=&queues[1],.avc_qp_limits={&control[1]},.state=3}};
    assert(!wave5_apply_enc_changes(&inst[0]) && !calls && !locked);
    inst[0].requested=4000000; inst[0].enc_change_mask=0x300;
    inst[0].requested=2000000; inst[0].enc_change_mask|=1;
    assert(!wave5_apply_enc_changes(&inst[0]) && calls==1 && last_mask==0x301);
    assert(inst[0].active==2000000 && !inst[0].enc_change_mask && !locked);
    assert(!wave5_apply_enc_changes(&inst[0]) && calls==1);
    inst[0].requested=1000000; inst[0].enc_change_mask=0x100;
    command_error=-EIO;
    assert(wave5_apply_enc_changes(&inst[0])==-EIO && !locked);
    assert(inst[0].active==2000000 && inst[0].enc_change_mask==0x100 && inst[0].state==4);
    assert(queues[0].src.errors==1 && queues[0].dst.errors==1);
    assert(queues[0].src.owned==3 && queues[0].dst.owned==2);
    command_error=0; build_error=-ERANGE; inst[1].enc_change_mask=0x100;
    assert(wave5_apply_enc_changes(&inst[1])==-ERANGE && calls==2 && !locked);
    assert(queues[1].src.errors==1 && queues[1].dst.errors==1 && inst[1].state==4);
    assert(queues[1].src.owned==1 && queues[1].dst.owned==1 && !inst[1].active);
    puts("Wave5 deferred changes: coalescing, queue wakeup, DMA retention and instance isolation passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-apply-') as directory:
    code = Path(directory) / 'apply.c'
    binary = Path(directory) / 'apply'
    code.write_text(apply_mock + apply_change + apply_checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
