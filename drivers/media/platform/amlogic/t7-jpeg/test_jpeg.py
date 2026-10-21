#!/usr/bin/env python3
"""Exercise actual JPEG programming, scan headers and DMA boundaries offline."""
from pathlib import Path
import subprocess
import tempfile

directory = Path(__file__).parent
source = (directory / 't7-jpeg.c').read_text()
defines = '\n'.join(line for line in source.splitlines()
                    if line.startswith('#define ') and not line.startswith('#define JPEG_IOCTL'))
registers = source[source.index('static u32 rd('):source.index('static irqreturn_t jpeg_irq')]
codec = source[source.index('static const u8 zigzag_order'):source.index('static long jpeg_ioctl')]
prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define BIT(n) (1u << (n))
#define PAGE_SIZE 4096u
#define PAGE_ALIGN(n) (((n) + 4095u) & ~4095u)
#define SZ_16M (16u * 1024u * 1024u)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define clamp(x,a,b) min(((x) < (a) ? (a) : (x)), (b))
#define DIV_ROUND_CLOSEST(n,d) (((n) + (d)/2) / (d))
#define WRITE_ONCE(p,v) ((p) = (v))
#define dma_wmb() ((void)0)
#define ALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define dma_get_cache_alignment() 64u
enum dma_data_direction { DMA_BIDIRECTIONAL };
struct device { int unused; };
struct dma_buf { u64 size; };
struct scatterlist { dma_addr_t dma; };
struct sg_table { struct scatterlist *sgl; };
#define sg_dma_address(sg) ((sg)->dma)
struct t7_jpeg { void *regs; bool powered; u32 stage, bytes; struct device *dev; };
struct t7_jpeg_file {
    struct t7_jpeg *jpeg;
    void *mapping;
    dma_addr_t dma;
    struct dma_buf *input;
    struct sg_table *sgt;
    u32 header_bytes;
    size_t output_cpu_bytes, input_device_bytes;
    bool running;
};
struct sync_event { bool cpu; dma_addr_t address; size_t size; };
static struct sync_event events[512];
static unsigned int event_count;
static void record_sync(bool cpu, dma_addr_t address, size_t size) {
    assert(size && event_count < 512);
    events[event_count++] = (struct sync_event){cpu, address, size};
}
static void dma_sync_single_for_device(struct device *dev, dma_addr_t address,
        size_t size, enum dma_data_direction direction) {
    (void)dev; assert(direction == DMA_BIDIRECTIONAL); record_sync(false, address, size);
}
static void dma_sync_single_for_cpu(struct device *dev, dma_addr_t address,
        size_t size, enum dma_data_direction direction) {
    (void)dev; assert(direction == DMA_BIDIRECTIONAL); record_sync(true, address, size);
}
static u8 mmio[65536];
static unsigned int writes;
static u32 readl(void *p) {
    assert((u8 *)p >= mmio && (u8 *)p + 4 <= mmio + sizeof(mmio));
    u32 value; memcpy(&value, p, 4); return value;
}
static void writel(u32 v, void *p) {
    assert((u8 *)p >= mmio && (u8 *)p + 4 <= mmio + sizeof(mmio));
    writes++; memcpy(p, &v, 4);
}
'''
checks = r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    struct t7_jpeg j = {.regs=mmio, .powered=true};
    struct t7_jpeg_file f = {.jpeg=&j, .dma=0x10000000};
    f.output_cpu_bytes = OUTPUT_SIZE;
    f.mapping = calloc(1, MAPPING_SIZE);
    assert(f.mapping);
    u32 cmd[30] = {0};
    cmd[1]=3; cmd[2]=4; cmd[3]=16; cmd[4]=16;
    cmd[5]=16*16*3/2; cmd[7]=80;
    cmd[11]=cmd[12]=cmd[13]=cmd[14]=16;
    for (unsigned int quality=1; quality<=100; quality++) {
        cmd[7]=quality;
        unsigned int previous=event_count;
        size_t previous_output=f.output_cpu_bytes;
        assert(start_frame(&f, cmd)==0);
        assert(event_count == previous + 2 && !f.output_cpu_bytes);
        assert(!events[previous].cpu && events[previous].address == f.dma + OUTPUT_OFFSET);
        assert(events[previous].size == previous_output);
        assert(!events[previous+1].cpu && events[previous+1].address == f.dma);
        assert(events[previous+1].size == 384 && f.input_device_bytes == 384);
        assert(f.running && f.header_bytes < HEADER_SIZE && !(f.header_bytes & 7));
        assert(rd(&j, MFDIN_Y) == f.dma);
        assert(rd(&j, MFDIN_U) == f.dma + 256);
        assert(rd(&j, VB_END) == f.dma + MAPPING_SIZE - 1);
        assert(rd(&j, MPSR)==1);
        unsigned int before=writes;
        assert(start_frame(&f, cmd)==-EBUSY && writes==before);
        assert(event_count == previous + 2);
        /* Alternate growing and shrinking results; untouched output stays
         * device-owned rather than flushing the entire 16 MiB each frame. */
        size_t bytes = quality & 1 ? 1031 : 32769;
        finish_dma(&f, bytes);
        assert(event_count == previous + 4 && !f.input_device_bytes);
        assert(events[previous+2].cpu && events[previous+2].address == f.dma);
        assert(events[previous+2].size == 384);
        assert(events[previous+3].cpu && events[previous+3].address == f.dma + OUTPUT_OFFSET);
        assert(events[previous+3].size == ALIGN(bytes, 64u));
        assert(f.output_cpu_bytes == ALIGN(bytes, 64u));
        f.running=false;
    }
    FILE *out=fopen(argv[1], "wb"); assert(out);
    assert(fwrite(f.mapping + HEADER_OFFSET, 1, f.header_bytes, out)==f.header_bytes);
    /* One flat, zero-DC 4:2:0 MCU under the standard Huffman tables. */
    const u8 scan[]={0x28,0xa2,0x8a,0x00,0xff,0xd9};
    assert(fwrite(scan, 1, sizeof(scan), out)==sizeof(scan)); fclose(out);
    for (unsigned int field=0; field<30; field++) {
        if (field!=0 && field!=1 && field!=2 && field!=3 && field!=4 &&
            field!=5 && field!=7 && field!=11 && field!=12 && field!=13 && field!=14) continue;
        u32 saved=cmd[field]; cmd[field]=0xffffffff;
        unsigned int before=writes;
        assert(start_frame(&f, cmd)==-EINVAL && writes==before && !f.running);
        cmd[field]=saved;
    }
    struct scatterlist sg={.dma=0xfffffe00};
    struct sg_table sgt={.sgl=&sg};
    struct dma_buf input={.size=384};
    f.input=&input; f.sgt=&sgt;
    unsigned int imported_start=event_count;
    assert(start_frame(&f, cmd)==0);
    assert(event_count == imported_start + 1 && !f.input_device_bytes);
    finish_dma(&f, 1777); f.running=false;
    assert(event_count == imported_start + 2);
    assert(events[event_count-1].cpu && events[event_count-1].size == ALIGN(1777, 64u));
    sg.dma=0xffffff00;
    unsigned int before=writes;
    assert(start_frame(&f, cmd)==-EINVAL && writes==before);
    sg.dma=0x20000000; input.size=383;
    assert(start_frame(&f, cmd)==-EINVAL && writes==before);
    j.powered=false;
    assert(start_frame(&f, cmd)==-EPIPE && writes==before);
    free(f.mapping);
    puts("PASS: 100 qualities, JPEG headers, command bounds, busy/unpowered, DMA extent/4GiB");
    puts("PASS: first DMA handoff, changing output lengths and imported/system-memory ownership");
}
'''
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    code = temp / 'jpeg.c'
    code.write_text(prefix + defines + '\n' + (directory / 'jpeg-tables.h').read_text()
                    + '\n' + registers + codec + checks)
    executable, image = temp / 'jpeg', temp / 'flat.jpg'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(code), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(image)], check=True)
    from PIL import Image
    with Image.open(image) as decoded:
        decoded.load()
        assert decoded.size == (16, 16)
        assert decoded.convert('RGB').getextrema() == ((128, 128),) * 3
    print('PASS: generated header and zero-DC scan decode to a complete neutral 16x16 image')
