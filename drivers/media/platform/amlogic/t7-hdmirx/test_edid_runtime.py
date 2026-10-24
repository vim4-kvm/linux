#!/usr/bin/env python3
"""Run actual EDID operations across leases, partial reads and failed writes."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define T7_RX_EDID_CAPACITY 512
#define T7_RX_EDID_BLOCK_SIZE 128
#define min(a,b) ((a)<(b)?(a):(b))
#define WRITE_ONCE(place, value) ((place) = (value))
struct link { int lock; unsigned int users, video_samples; bool ready; };
struct t7_hdmirx { struct link *link; struct { size_t size; } edid_blob;
    u8 edid[512]; u32 saved_reset, saved_port_sel;
    bool ddc_active, core_active, uhd60; };
struct device { struct t7_hdmirx *data; };
struct v4l2_edid { unsigned int pad, start_block, blocks; u8 *edid; };
static int depth, programs, failures, stops;
static bool hpd = true;
static void t7_hdmirx_set_hpd(struct t7_hdmirx *rx, bool high) { (void)rx; hpd=high; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void mutex_lock(int *lock) { (void)lock; assert(!depth); depth++; }
static void mutex_unlock(int *lock) { (void)lock; assert(depth==1); depth--; }
static int t7_hdmirx_ddc_start(struct t7_hdmirx *rx, const u8 *data, size_t size) {
    assert(depth==1); programs++; rx->saved_reset=999; rx->saved_port_sel=999;
    rx->edid_blob.size=0; rx->ddc_active=true; hpd=false;
    if (failures) { failures--; rx->ddc_active=false; return -EIO; }
    memcpy(rx->edid,data,size); rx->edid_blob.size=size; return 0;
}
static void t7_hdmirx_ddc_stop(struct t7_hdmirx *rx) {
    stops++; rx->edid_blob.size=0; rx->ddc_active=false; hpd=false;
}
'''
checks = r'''
static void checksum(u8 *data) {
    unsigned int sum=0; for (int i=0;i<127;i++) sum+=data[i]; data[127]=-sum;
}
int main(void) {
    u8 table[256]={0}, changed[256], output[512]={0};
    const u8 header[]={0,255,255,255,255,255,255,0};
    memcpy(table,header,8); table[126]=1; checksum(table); checksum(table+128);
    struct link link={.users=1,.ready=true,.video_samples=3};
    struct t7_hdmirx rx={.link=&link,.edid_blob.size=256,.saved_reset=3,.saved_port_sel=7,
                        .ddc_active=true,.core_active=true,.uhd60=true};
    memcpy(rx.edid,table,256);
    struct device dev={.data=&rx};
    struct v4l2_edid request={.blocks=2,.edid=table};
    assert(t7_hdmirx_set_edid(&dev,&request)==0 && programs==0);
    assert(link.ready && link.video_samples==3);
    memcpy(changed,table,256); changed[20]=1; checksum(changed); request.edid=changed;
    assert(t7_hdmirx_set_edid(&dev,&request)==-EBUSY && programs==0);
    assert(link.ready && link.video_samples==3);
    link.users=0;
    assert(t7_hdmirx_set_edid(&dev,&request)==0 && programs==1);
    assert(!link.ready && !link.video_samples);
    assert(hpd);
    assert(rx.saved_reset==3 && rx.saved_port_sel==7 && !memcmp(rx.edid,changed,256));
    request.edid=table; failures=1; link.ready=true; link.video_samples=3;
    assert(t7_hdmirx_set_edid(&dev,&request)==-EIO && programs==3);
    assert(!link.ready && !link.video_samples);
    assert(rx.edid_blob.size==256 && !memcmp(rx.edid,changed,256));
    assert(hpd);
    assert(rx.saved_reset==3 && rx.saved_port_sel==7);
    struct v4l2_edid read={0};
    assert(t7_hdmirx_get_edid(&dev,&read)==0 && read.blocks==2);
    read.blocks=4; read.start_block=1; read.edid=output;
    assert(t7_hdmirx_get_edid(&dev,&read)==0 && read.blocks==1);
    assert(!memcmp(output,changed+128,128));
    read.start_block=2; assert(t7_hdmirx_get_edid(&dev,&read)==-ENODATA);
    request.blocks=5; assert(t7_hdmirx_set_edid(&dev,&request)==-E2BIG);
    request.blocks=2; table[127]^=1;
    assert(t7_hdmirx_set_edid(&dev,&request)==-EINVAL && programs==3);
    request.blocks=0; link.users=1;
    assert(t7_hdmirx_set_edid(&dev,&request)==-EBUSY && stops==0);
    link.users=0; assert(t7_hdmirx_set_edid(&dev,&request)==0 && stops==1);
    assert(!hpd);
    request.blocks=2; request.edid=changed;
    assert(t7_hdmirx_set_edid(&dev,&request)==0 && hpd);
    table[127]^=1; request.edid=table; failures=2;
    assert(t7_hdmirx_set_edid(&dev,&request)==-EIO && !hpd && !rx.ddc_active);
    assert(!depth); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='t7-edid-runtime-') as directory:
    root = Path(directory)
    functions = ''.join(function(s) for s in ['static int t7_hdmirx_validate_edid(',
        'int t7_hdmirx_get_edid(', 'int t7_hdmirx_set_edid('])
    (root / 'test.c').write_text(harness + functions + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('Runtime EDID leases, readback and failed-write recovery passed')
