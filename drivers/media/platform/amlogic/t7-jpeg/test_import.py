#!/usr/bin/env python3
"""Compile actual DMA import/release paths with tracked reservations and ownership."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-jpeg.c').read_text()
functions = source[source.index('static void release_input'):source.index('static const u8 zigzag_order')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
typedef uint64_t dma_addr_t;
#define HZ 100
#define DMA_TO_DEVICE 1
#define DMA_RESV_USAGE_WRITE 1
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
struct dma_buf { void *resv; };
struct dma_buf_attachment { int unused; };
struct scatterlist { dma_addr_t address; unsigned int length; };
struct sg_table { struct scatterlist *sgl; unsigned int nents; };
struct t7_jpeg { void *dev; };
struct t7_jpeg_file {
    struct t7_jpeg *jpeg;
    struct dma_buf *input;
    struct dma_buf_attachment *attachment;
    struct sg_table *sgt;
    bool running;
};
static struct dma_buf buffer;
static struct dma_buf_attachment attachment;
static struct scatterlist segments[2];
static struct sg_table table={segments,2};
static int references, attachments, mappings, error_at, waited;
static struct dma_buf *dma_buf_get(int fd) {
    assert(fd==7);
    if (error_at==1) return ERR_PTR(-EBADF);
    references++; return &buffer;
}
static int dma_resv_wait_timeout(void *resv, int usage, bool interruptible, int timeout) {
    assert(resv==buffer.resv && usage==DMA_RESV_USAGE_WRITE && interruptible && timeout==HZ);
    waited++;
    if (error_at==2) return -EINTR;
    if (error_at==3) return 0;
    return 1;
}
static struct dma_buf_attachment *dma_buf_attach(struct dma_buf *b, void *dev) {
    assert(b==&buffer && dev==NULL && waited);
    if (error_at==4) return ERR_PTR(-ENOMEM);
    attachments++; return &attachment;
}
/* Only the unlocked helpers may be called: these paths do not own the
 * exporter's reservation. Their locked counterparts deliberately do not
 * exist in this harness, so a regression also fails compilation. */
static struct sg_table *dma_buf_map_attachment_unlocked(struct dma_buf_attachment *a, int direction) {
    assert(a==&attachment && direction==DMA_TO_DEVICE && attachments==1);
    if (error_at==5) return ERR_PTR(-EIO);
    mappings++; return &table;
}
static void dma_buf_unmap_attachment_unlocked(struct dma_buf_attachment *a, struct sg_table *t, int direction) {
    assert(a==&attachment && t==&table && direction==DMA_TO_DEVICE && mappings==1);
    mappings--;
}
static void dma_buf_detach(struct dma_buf *b, struct dma_buf_attachment *a) {
    assert(b==&buffer && a==&attachment && !mappings && attachments==1); attachments--;
}
static void dma_buf_put(struct dma_buf *b) {
    assert(b==&buffer && !attachments && !mappings && references==1); references--;
}
#define sg_dma_address(sg) ((sg)->address)
#define sg_dma_len(sg) ((sg)->length)
#define for_each_sgtable_dma_sg(t,sg,i) for ((i)=0;(i)<(t)->nents && (((sg)=&(t)->sgl[i]),true);(i)++)
'''
checks = r'''
int main(void) {
    struct t7_jpeg j={0};
    struct t7_jpeg_file f={.jpeg=&j};
    segments[0]=(struct scatterlist){0x10000000,4096};
    segments[1]=(struct scatterlist){0x10001000,4096};
    for (error_at=1;error_at<=5;error_at++) {
        waited=0;
        assert(import_input(&f,7)<0 && !f.input);
        assert(!references && !attachments && !mappings);
        release_input(&f);
    }
    error_at=0;
    segments[1].address+=4096;
    assert(import_input(&f,7)==-EINVAL && !references && !mappings && !attachments);
    segments[0].address=0xffffe000;segments[1].address=0xfffff000;
    assert(import_input(&f,7)==0); release_input(&f);
    segments[1].length=4097;
    assert(import_input(&f,7)==-ERANGE && !references && !mappings && !attachments);
    segments[1].length=4096;
    for (int cycle=0;cycle<1000;cycle++) {
        assert(import_input(&f,7)==0);
        assert(import_input(&f,7)==-EBUSY && references==1 && mappings==1);
        release_input(&f);
        assert(!f.input && !f.attachment && !f.sgt && !references && !mappings && !attachments);
    }
    f.running=true;
    assert(import_input(&f,7)==-EBUSY && !references);
    puts("PASS: reservation API, producer fence, partial failures, contiguous/4GiB bounds, 1000 import/release cycles");
}
'''
with tempfile.TemporaryDirectory() as temp:
    code, executable = Path(temp) / 'import.c', Path(temp) / 'import'
    code.write_text(prefix + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
