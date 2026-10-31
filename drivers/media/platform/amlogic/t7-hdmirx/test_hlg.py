#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for the real HLG writer, source tables and lease transitions.

Optional argument: fixed vendor set_hdr2_v0.c at common_drivers commit
8648a716befda5ec2837a7764cd61b8493db8d99. No floating-point regeneration is
used: preserve the vendor's encoded EOTF, integer gain and OETF quantization.
"""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
text = (root / 't7-vdin-hlg.h').read_text()
hashes = {
    'eotf': '887e6841dfeb378230a0be770dc646801e6758310c4e72333872e41bebaf2150',
    'gain': '84f874e72b917f4fc32032bc8f6a1bdc44b9a91553d7eea841e0533accc38ca1',
    'oetf': 'bcb715148098dffe8c2d1fbf867ae1e80e5dc7f7fb03b401100ee40ca0739099',
    'cgain': '7b23b31657f2ac921ce26347da579deefe4b9543050772b090dddb357454431c',
    'gamut': '4650784da5a8632231c0a332aed110705c4c884928c366259c9e782f68005267',
}


def array(source, name):
    source = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
    body = re.search(r'\b' + name + r'\[[^]]+\]\s*=\s*\{([^}]+)\}', source)[1]
    return [int(v, 0) for v in re.findall(r'-?(?:0x[0-9a-fA-F]+|\d+)', body)]


pq = (root / 't7-vdin-hdr.h').read_text()
tables = {name: array(text, 't7_hlg_' + name) for name in hashes if name != 'oetf'}
tables['oetf'] = array(pq, 't7_hdr_srgb')
for name, values in tables.items():
    assert hashlib.sha256(struct.pack('<' + 'i' * len(values), *values)).hexdigest() == hashes[name]
if len(sys.argv) == 2:
    vendor = Path(sys.argv[1]).read_text()
    for name, original in [('eotf', 'eo_y_lut_hlg'), ('gain', 'oo_y_lut_hlg_sdr'),
                           ('cgain', 'cgain_lut1'),
                           ('gamut', 'ncl_2020_709')]:
        values = array(vendor, original)
        if name == 'cgain':
            values = [v - 1 for v in values]
        assert values == tables[name], name

fixture = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t u32;
#define BIT(x) (1u << (x))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define VDIN_HDR_MATRIX_IN 0x12bb
#define VDIN_HDR_MATRIX_OUT 0x12bc
#define VDIN_HDR_CTRL 0x1280
#define VDIN_MATRIX_CTRL 0x1210
struct t7_vdin {int unused;};
static void t7_vdin_write(struct t7_vdin *v, u32 reg, u32 data)
{(void)v; printf("%x %x\n", reg, data);}
static void t7_vdin_hdr_matrix(struct t7_vdin *v,u32 base,const int *c)
{
 for(unsigned i=0;i<4;i++) t7_vdin_write(v,base+i,((u32)c[2*i]<<16)|(c[2*i+1]&0x1fff));
 t7_vdin_write(v,base+4,c[8]&0x1fff);
}
#include "t7-vdin-hlg.h"
int main(void) {
 struct t7_vdin v={0};
 assert(t7_hdr_eotf_supported(2) && t7_hdr_eotf_supported(3));
 assert(!t7_hdr_eotf_supported(0) && !t7_hdr_eotf_supported(1) && !t7_hdr_eotf_supported(255));
 for(unsigned old=0;old<4;old++) for(unsigned cur=0;cur<4;cur++) {
  bool a=t7_hdr_eotf_supported(old),b=t7_hdr_eotf_supported(cur);
  assert(t7_hdr_encoding_changed(b,cur,a,old)==(a!=b||(a&&b&&old!=cur)));
 }
 t7_vdin_hlg_configure(&v);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    declaration = re.search(r'static const u32 t7_hdr_srgb\[149\] = \{[^}]+\};', pq)[0]
    c.write_text(fixture.replace('#include "t7-vdin-hlg.h"', declaration + '\n#include "t7-vdin-hlg.h"'))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', '-I', str(root), str(c), '-o', str(exe)], check=True)
    writes = [tuple(int(v, 16) for v in line.split()) for line in subprocess.check_output([str(exe)], text=True).splitlines()]

def packed(values, shift):
    values = [v >> shift for v in values]
    return [values[i] | values[i + 1] << 16 for i in range(0, len(values) - 1, 2)] + [values[-1]]

for name, address, data, shift in [('gain', 0x12a6, 0x12a7, 0), ('oetf', 0x12a0, 0x12a1, 2),
                                    ('cgain', 0x12a2, 0x12a3, 0)]:
    assert [v for r, v in writes if r == address] == [0]
    assert [v for r, v in writes if r == data] == packed(tables[name], shift)
assert [v for r, v in writes if r == 0x129f] == tables['eotf']
regs = dict(writes)
for i in range(4):
    assert regs[0x12b1 + i] == ((tables['gamut'][2 * i] & 0xffff) |
                                (tables['gamut'][2 * i + 1] & 0xffff) << 16)
assert regs[0x12b5] == tables['gamut'][-1]
allowed = (set(range(0x1282, 0x1287)) | set(range(0x128a, 0x1293)) |
           set(range(0x1296, 0x129a)) | set(range(0x129e, 0x12b6)) |
           {0x12bb, 0x12bc, 0x1280, 0x1210})
assert all(reg in allowed for reg, value in writes)
assert [i for i, (reg, value) in enumerate(writes) if reg == 0x1280] == [len(writes)-2]
assert regs[0x12b0] == (1 << 4) | 11
assert regs[0x12a8] == 0  # HLG Ys mode; PQ/max-RGB would be 1.
assert regs[0x12aa] == (10 << 24) | (9 << 20) | (10 << 16) | 1024
assert regs[0x12ae] == 694 << 16 | 269 and regs[0x12af] == 61
assert regs[0x12a4] == (627 * 4 << 16) | (186 * 4)
assert regs[0x12a5] == 0x400 << 16 | 63 * 4
assert writes[-1] == (0x1210, 1) and writes[-2][0] == 0x1280
print('HLG vendor tables, encoded port words and lease selection: PASS')

# Exercise the actual write-completion handoff: first-frame discard, ordinary
# completion, stopping and AXI failure must never publish an unsafe frame.
vdin = (root / 't7-vdin-test.c').read_text()
start = vdin.index('static void t7_vdin_ring_done(')
done = vdin[start:vdin.index('\nstatic irqreturn_t', start)]
fixture = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#define BIT(x) (1u<<(x))
#define VDIN_WR_CTRL 0
#define VDIN_WR_STATUS 1
#define VDIN_WR_REQ 1
#define VDIN_VCP_WR 2
enum {VDIN_SLOT_FREE,VDIN_SLOT_READY,VDIN_SLOT_WRITING};
struct t7_vdin_slot {int state; uint64_t timestamp; unsigned sequence;};
struct t7_vdin {struct t7_vdin_slot slots[1]; unsigned active_slot,frame_irqs,ring_sequence,ring_pauses;
 bool ring_running,dma_idle,ring_stopping,ring_discard; int ring_error,done,buffer_wait;};
static unsigned axi=1;
static void t7_vdin_update(struct t7_vdin *v,int r,int m,int d){(void)v;(void)r;(void)m;(void)d;}
static unsigned t7_vdin_read(struct t7_vdin *v,int r){(void)v;(void)r;return axi;}
static uint64_t ktime_get_ns(void){return 100;}
static int t7_vdin_ring_free_slot(struct t7_vdin *v){(void)v;return -1;}
static void t7_vdin_ring_arm(struct t7_vdin *v,int n){(void)n;v->ring_running=true;}
static void complete(int *p){(*p)++;}
static void wake_up(int *p){(*p)++;}
/* DONE */
int main(void){
 struct t7_vdin v={.ring_running=true,.ring_discard=true,.slots={{.state=VDIN_SLOT_WRITING}}};
 t7_vdin_ring_done(&v); assert(v.slots[0].state==VDIN_SLOT_FREE && !v.ring_discard && !v.ring_sequence);
 v.ring_running=true;v.slots[0].state=VDIN_SLOT_WRITING;
 t7_vdin_ring_done(&v);assert(v.slots[0].state==VDIN_SLOT_READY && v.ring_sequence==1);
 v.ring_running=true;v.slots[0].state=VDIN_SLOT_WRITING;v.ring_stopping=true;
 t7_vdin_ring_done(&v);assert(v.slots[0].state==VDIN_SLOT_WRITING && v.ring_sequence==1);
 v.ring_running=true;v.ring_stopping=false;axi=0;
 t7_vdin_ring_done(&v);assert(v.slots[0].state==VDIN_SLOT_WRITING && v.ring_error==-EIO);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(fixture.replace('/* DONE */', done))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Actual frame discard/stop/AXI-error handoff: PASS')

# Compile real packet acquisition and HDR qualification against a fake COR/TOP.
# This exercises malformed packets, packet freshness and limited-range gating.
rx = (root / 't7-hdmirx.c').read_text()
start = rx.index('static bool t7_hdmirx_packet_valid(')
functions = rx[start:rx.index('\nstatic bool t7_hdmirx_sdr420_input', start)]
fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
typedef uint8_t u8; typedef uint64_t u64;
#define BIT(x) (1u<<(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define T7_RX_TOP_HPD_PWR5V 0
#define T7_RX_TOP_TMDS_ALIGN_STAT 4
#define T7_RX_COR_COLOR_DEPTH 0
struct t7_hdmirx {unsigned char *cor,*top; bool hdr_capture,dynamic_modes;
 unsigned port; unsigned long long drm_received_ns; u8 drm_eotf;};
static unsigned char cor[0x1200],top[8];
static unsigned now=100;
static bool tear;
static unsigned reads;
static u8 readb(unsigned char *p) {
 if(tear && p==cor+0x11c4 && ++reads==2) return *p ^ 1;
 return *p;
}
static unsigned readl(unsigned char *p) {unsigned v;memcpy(&v,p,4);return v;}
static void writeb(u8 v,unsigned char *p){(void)v;(void)p;}
static u64 ktime_get_ns(void){return now;}
#include "t7-hlg-state.h"
/* FUNCTIONS */
static void checksum(u8 *p,unsigned n){p[3]=0;unsigned sum=0;for(unsigned i=0;i<n;i++)sum+=p[i];p[3]=-sum;}
int main(void) {
 struct t7_hdmirx r={.cor=cor,.top=top,.hdr_capture=true,.dynamic_modes=true};
 unsigned power=BIT(20),align=1;memcpy(top,&power,4);memcpy(top+4,&align,4);
 cor[0]=5;cor[0x1134]=BIT(4);
 u8 *drm=cor+0x11c0,*avi=cor+0x1140;
 drm[0]=0x87;drm[1]=1;drm[2]=26;drm[4]=3;checksum(drm,30);
 avi[0]=0x82;avi[1]=2;avi[2]=13;avi[4]=3<<5;avi[5]=3<<6;avi[6]=6<<4;checksum(avi,17);
 t7_hdmirx_update_drm(&r);assert(r.drm_eotf==3 && t7_hdmirx_hdr_input(&r));
 cor[0x1134]=0;now=1000000200; t7_hdmirx_update_drm(&r);assert(!t7_hdmirx_hdr_input(&r));
 now=200;cor[0x1134]=BIT(4);drm[3]++;t7_hdmirx_update_drm(&r);assert(!t7_hdmirx_hdr_input(&r));
 checksum(drm,30);tear=true;t7_hdmirx_update_drm(&r);assert(!t7_hdmirx_hdr_input(&r));tear=false;
 drm[5]=1;checksum(drm,30);t7_hdmirx_update_drm(&r);assert(!t7_hdmirx_hdr_input(&r));
 drm[5]=0;checksum(drm,30);t7_hdmirx_update_drm(&r);assert(t7_hdmirx_hdr_input(&r));
 avi[8]=1<<6;checksum(avi,17);assert(!t7_hdmirx_hdr_input(&r));
 avi[8]=0;checksum(avi,17);r.dynamic_modes=false;assert(!t7_hdmirx_hdr_input(&r));
 r.dynamic_modes=true;power=0;memcpy(top,&power,4);t7_hdmirx_update_drm(&r);assert(!t7_hdmirx_hdr_input(&r));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    c.write_text(fixture.replace('/* FUNCTIONS */', functions))
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Actual HDMI DRM/AVI parsing, expiry and qualification: PASS')
