#!/usr/bin/env python3
"""Compile actual UAC2 GET_CUR handling; reject access to disabled runtimes."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('f_uac2.c').read_text()
start = source.index('static int\nin_rq_cur(')
function = source[start:source.index('\n}\n', start) + 3]
ids = '\n'.join(row for row in source.splitlines()
                if row.startswith('#define USB_') and '_ID' in row)
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
#define le16_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define dev_err(...) ((void)0)
#define FUIN_EN(opts) 0
#define FUOUT_EN(opts) 0
#define UAC2_CS_CONTROL_SAM_FREQ 1
#define UAC2_CS_CONTROL_CLOCK_VALID 2
#define UAC_FU_MUTE 1
#define UAC_FU_VOLUME 2
struct usb_request { void *buf; };
struct usb_composite_dev { struct usb_request *req; };
struct usb_configuration { struct usb_composite_dev *cdev; };
struct usb_function { struct usb_configuration *config; };
struct usb_ctrlrequest { u16 wLength, wIndex, wValue; };
struct f_uac2_opts { int p_chmask, c_chmask; };
struct g_audio { struct f_uac2_opts opts; };
struct cntrl_cur_lay3 { u32 dCUR; };
struct cntrl_cur_lay2 { u16 wCUR; };
static struct { u8 bClockID; } in_clk_src_desc={5}, out_clk_src_desc={6};
static struct { u8 bUnitID; } in_feature={7}, out_feature={8};
#define in_feature_unit_desc (&in_feature)
#define out_feature_unit_desc (&out_feature)
static struct g_audio audio;
static unsigned reads;
static struct g_audio *func_to_g_audio(struct usb_function *fn) { (void)fn; return &audio; }
static struct f_uac2_opts *g_audio_to_uac2_opts(struct g_audio *a) { return &a->opts; }
static int u_audio_get_playback_srate(struct g_audio *a, u32 *v) {
    assert(a->opts.p_chmask); reads++; *v=48000; return 0;
}
static int u_audio_get_capture_srate(struct g_audio *a, u32 *v) {
    assert(a->opts.c_chmask); reads++; *v=44100; return 0;
}
static void u_audio_get_mute(struct g_audio *a, unsigned p, unsigned *v) {
    (void)a; (void)p; (void)v; assert(0);
}
static void u_audio_get_volume(struct g_audio *a, unsigned p, s16 *v) {
    (void)a; (void)p; (void)v; assert(0);
}
'''
checks = r'''
int main(void) {
    u8 payload[8];
    struct usb_request req = {.buf=payload};
    struct usb_composite_dev cdev = {.req=&req};
    struct usb_configuration config = {.cdev=&cdev};
    struct usb_function fn = {.config=&config};
    for (int playback=0; playback<2; playback++) for (int capture=0; capture<2; capture++) {
        audio.opts.p_chmask=playback*3; audio.opts.c_chmask=capture*3;
        for (unsigned direction=0; direction<2; direction++) for (unsigned selector=1; selector<=3; selector++) {
            const unsigned id=direction ? USB_OUT_CLK_ID : USB_IN_CLK_ID;
            const int enabled=direction ? capture : playback;
            struct usb_ctrlrequest cr={.wLength=8, .wIndex=id<<8, .wValue=selector<<8};
            reads=0; memset(payload, 0xa5, sizeof(payload));
            int result=in_rq_cur(&fn, &cr);
            if (!enabled || selector==3) {
                assert(result==-EOPNOTSUPP && reads==0 && payload[0]==0xa5);
            } else if (selector==UAC2_CS_CONTROL_SAM_FREQ) {
                u32 value; memcpy(&value, payload, sizeof(value));
                assert(result==4 && reads==1 && value==(direction ? 44100u : 48000u));
                assert(payload[4]==0xa5);
                cr.wLength=2; memset(payload, 0xa5, sizeof(payload));
                assert(in_rq_cur(&fn, &cr)==2 && payload[2]==0xa5);
            } else {
                assert(result==1 && reads==0 && payload[0]==1 && payload[1]==0xa5);
            }
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='uac2-controls-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness + ids + '\n' + function + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: playback/capture-only UAC2 rates, disabled clocks and short responses')
