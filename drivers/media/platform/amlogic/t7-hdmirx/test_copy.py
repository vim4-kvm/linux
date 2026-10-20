#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare a SIMD candidate with the actual kernel memcpy_generic offline.

Linux ARM64 only. Default runs correctness checks; --output retains a standalone
binary. Its --benchmark /dev/videoN mode allocates idle VDIN MMAP buffers but
never starts capture. No application may own the capture queue during this run.
"""
import argparse
import hashlib
from pathlib import Path
import platform
import subprocess
import tempfile

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
if platform.system() != 'Linux' or platform.machine() not in ('aarch64', 'arm64'):
    parser.error('Run in a Linux ARM64 build environment')
reference = (root.parents[4] / 'arch/arm64/lib/memcpy.S').read_text().split('#ifdef CONFIG_AS_HAS_MOPS')[0]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define SZ_256K (256U*1024)
static bool simd_copy;
void t7_vdin_reference_copy(void *dst, const void *src, size_t n);
void t7_vdin_copy_blocks(void *dst, const void *src, size_t n);
static void t7_vdin_copy_frame(void *dst, const void *src, size_t n) {
    size_t blocks=simd_copy ? n&~(size_t)127 : 0;
    if (blocks) t7_vdin_copy_blocks(dst,src,blocks);
    memcpy((char *)dst+blocks,(const char *)src+blocks,n-blocks);
}
'''
checks = r'''
static void check(size_t n, bool simd) {
    size_t pages=(n+4095)/4096+2, length=pages*4096;
    unsigned char *src=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *dst=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(src!=MAP_FAILED && dst!=MAP_FAILED);
    assert(!mprotect(src+length-4096,4096,PROT_NONE));
    assert(!mprotect(dst+length-4096,4096,PROT_NONE));
    unsigned char *s=src+length-4096-n, *d=dst+length-4096-n;
    for (size_t i=0;i<n;i++) s[i]=(i*17+13)&255;
    d[-1]=0xa5; simd_copy=simd;
    t7_vdin_copy_frame(d,s,n);
    assert(!memcmp(d,s,n) && d[-1]==0xa5);
    munmap(src,length); munmap(dst,length);
}
static double now(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return t.tv_sec*1e9+t.tv_nsec; }
static int order(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y);
}
static void benchmark(const char *node) {
    int fd=open(node,O_RDWR|O_NONBLOCK|O_CLOEXEC); assert(fd>=0);
    struct v4l2_capability cap;
    assert(!ioctl(fd,VIDIOC_QUERYCAP,&cap));
    assert(!strcmp((const char *)cap.driver,"t7-vdin-test"));
    struct v4l2_format format={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE};
    assert(!ioctl(fd,VIDIOC_G_FMT,&format));
    size_t size=format.fmt.pix.sizeimage;
    struct v4l2_requestbuffers request={.count=4,.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory=V4L2_MEMORY_MMAP};
    if (ioctl(fd,VIDIOC_REQBUFS,&request)) { perror("REQBUFS (stop viewers first)"); exit(1); }
    assert(request.count>=4);
    void *outputs[4]; size_t lengths[4]; unsigned char *sources[3];
    for (unsigned int i=0;i<4;i++) {
        struct v4l2_buffer b={.type=V4L2_BUF_TYPE_VIDEO_CAPTURE,.memory=V4L2_MEMORY_MMAP,.index=i};
        assert(!ioctl(fd,VIDIOC_QUERYBUF,&b)); assert(b.length>=size); lengths[i]=b.length;
        outputs[i]=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset);
        assert(outputs[i]!=MAP_FAILED);
    }
    for (unsigned int i=0;i<3;i++) { sources[i]=malloc(size); assert(sources[i]);
        for (size_t j=0;j<size;j++) sources[i][j]=(j*17+i)&255; }
    double times[2][9];
    for (unsigned int pass=0;pass<10;pass++) {
        // Alternate order between samples to limit drift bias; pass zero warms maps.
        for (unsigned int position=0;position<2;position++) {
            unsigned int variant=position^(pass&1);
            simd_copy=variant;
            double start=now();
            for (unsigned int frame=0;frame<32;frame++) {
                if (variant) t7_vdin_copy_frame(outputs[frame%4],sources[frame%3],size);
                else t7_vdin_reference_copy(outputs[frame%4],sources[frame%3],size);
            }
            double elapsed=(now()-start)/32;
            if (pass) times[variant][pass-1]=elapsed;
            for (unsigned int i=0;i<4;i++) {
                unsigned int last=28+i;
                assert(!memcmp(outputs[i],sources[last%3],size));
            }
        }
    }
    qsort(times[0],9,sizeof(double),order); qsort(times[1],9,sizeof(double),order);
    printf("{\"schema\":1,\"workload\":\"vdin-copy-v1\",\"source_sha256\":\"%s\",\"node\":\"%s\","
        "\"width\":%u,\"height\":%u,\"bytes\":%zu,\"samples\":9,\"iterations\":32,"
        "\"scalar_median_ns\":%.0f,\"simd_median_ns\":%.0f,"
        "\"scalar_p95_ns\":%.0f,\"simd_p95_ns\":%.0f,"
        "\"scope\":\"cached userspace source to actual idle VDIN vb2 mapping; no IRQ, source cache sync or kernel SIMD context costs\"}\n",
        COPY_SOURCE_SHA,node,format.fmt.pix.width,format.fmt.pix.height,size,times[0][4],times[1][4],times[0][8],times[1][8]);
    for (unsigned int i=0;i<4;i++) assert(!munmap(outputs[i],lengths[i]));
    request.count=0; assert(!ioctl(fd,VIDIOC_REQBUFS,&request)); close(fd);
    for (unsigned int i=0;i<3;i++) free(sources[i]);
}
int main(int argc,char **argv) {
    for (size_t n=0;n<272;n++) for (unsigned int mode=0;mode<2;mode++) check(n,mode!=0);
    size_t sizes[]={SZ_256K-1,SZ_256K,SZ_256K+1,1920*1080*3/2,3840*2160*3/2};
    for (size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) check(sizes[i],true);
    if (argc==3 && !strcmp(argv[1],"--benchmark")) benchmark(argv[2]);
    else { assert(argc==1); puts("VDIN SIMD candidate: bounds, tails and scalar fallback checks passed"); }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    build = Path(directory)
    (build / 'linux').mkdir()
    (build / 'asm').mkdir()
    (build / 'linux/linkage.h').write_text(
        '#define SYM_FUNC_START(n) .globl n; .type n,%function; n:\n'
        '#define SYM_FUNC_START_LOCAL(n) SYM_FUNC_START(n)\n'
        '#define SYM_FUNC_END(n) .size n,.-n\n')
    (build / 'asm/assembler.h').write_text('')
    (build / 'reference.S').write_text(reference)
    (build / 'test.c').write_text(harness + checks)
    binary = args.output.resolve() if args.output else build / 'copy'
    digest = hashlib.sha256((harness + checks + reference + (root / 't7-vdin-copy.S').read_text()).encode()).hexdigest()
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(build),
                    f'-DCOPY_SOURCE_SHA="{digest}"',
                    '-D__pi_memcpy_generic=t7_vdin_reference_copy', str(build / 'test.c'),
                    str(root / 't7-vdin-copy.S'), str(build / 'reference.S'),
                    '-Wl,-z,noexecstack', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
