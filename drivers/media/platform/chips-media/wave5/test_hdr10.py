#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real P010 setup and HDR10 serialization; parse SEI with FFmpeg.

No hardware is opened. Firmware SEI insertion and DMA sample order still need
hardware acceptance before any application advertises remote HDR support.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent
encoder = (root / 'wave5-vpu-enc.c').read_text()
setup = encoder.split('static int wave5_set_enc_openparam(', 1)[1]
setup = setup[setup.index('\tif (info->format == V4L2_PIX_FMT_P010)'):]
setup = setup[:setup.index('\n\tswitch (info->format)')]
api = (root / 'wave5-vpuapi.h').read_text()
formats = api[api.index('enum frame_buffer_format {'):]
formats = formats[:formats.index('};') + 2]
hardware = (root / 'wave5-hw.c').read_text()
endian = hardware[hardware.index('static u32 wave5_enc_source_endian('):
                  hardware.index('int wave5_vpu_encode(')]
core = root.parents[2] / 'v4l2-core' / 'v4l2-common.c'
# Extract the actual core entry so a broken bpp/divisor combination fails the
# buffer capacity test, rather than merely checking the driver's fourcc list.
entry = re.search(r'\{ \.format = V4L2_PIX_FMT_P010,.*?\.vdiv = \d+ \}',
                  core.read_text(), re.S)[0]
prefix = r'''
#include <time.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "wave5-hdr10.h"
#define W_HEVC_ENC 1
#define W_AVC_ENC 2
#define V4L2_PIXEL_ENC_YUV 1
#define PIC_SRC_ENDIANNESS_BIG_ENDIAN 15
struct v4l2_format_info { unsigned format, pixel_enc, mem_planes, comp_planes;
 unsigned bpp[4], bpp_div[4], hdiv, vdiv; };
struct vpu_instance { unsigned std; };
struct enc_wave_param { unsigned internal_bit_depth; };
struct enc_open_param { unsigned src_format; };
'''
checks = r'''
int main(void) {
 struct enc_open_param out={0};
 struct vpu_instance inst={.std=W_HEVC_ENC};
 struct enc_wave_param in={.internal_bit_depth=10};
 assert(!setup(&out,&inst,in));
 assert(out.src_format==FORMAT_420_P10_16BIT_LSB);
 /* Match the actual legacy SDK: convert_endian(17)=1, then invert its
  * low nibble for PIC_SRC_FORMAT. Eight-bit source bytes are unchanged. */
 assert(wave5_enc_source_endian(out.src_format,true)==((~17U)&15));
 assert(wave5_enc_source_endian(out.src_format,false)==15);
 assert(wave5_enc_source_endian(FORMAT_420,true)==15);
 assert(wave5_enc_source_endian(FORMAT_422,true)==15);
 in.internal_bit_depth=8;
 assert(setup(&out,&inst,in)==-EINVAL);
 in.internal_bit_depth=10; inst.std=W_AVC_ENC;
 assert(setup(&out,&inst,in)==-EINVAL);
 for (unsigned w=64; w<=4096; w+=32) {
  unsigned h=2160, y=w*p010.bpp[0], uv=(w/p010.hdiv)*p010.bpp[1];
  assert(y==w*2 && uv==y);
  assert(y*h+uv*(h/p010.vdiv)==w*h*3);
 }
 u8 sei[80];
 assert(!wave5_hdr10_sei(sei,NULL,NULL));
 struct v4l2_ctrl_hdr10_mastering_display m={
  .display_primaries_x={13250,7500,34000},
  .display_primaries_y={34500,3000,16000},
  .white_point_x=15635,.white_point_y=16450,
  .max_display_mastering_luminance=10000000,
  .min_display_mastering_luminance=1};
 struct v4l2_ctrl_hdr10_cll_info c={0};
 unsigned n=wave5_hdr10_sei(sei,&m,&c);
 assert(n<=sizeof(sei));
 fwrite(sei,1,n,stdout);
 c.max_content_light_level=1000; c.max_pic_average_light_level=400;
 n=wave5_hdr10_sei(sei,NULL,&c); fwrite(sei,1,n,stdout);
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-hdr10-') as temp:
    temp = Path(temp)
    source, exe = temp / 'test.c', temp / 'test'
    source.write_text(prefix + formats + '\n' + endian +
                      '\nstatic const struct v4l2_format_info p010=' +
                      entry + ';\nstatic int setup(struct enc_open_param *open_param, '
                      'struct vpu_instance *inst, struct enc_wave_param input) {\n'
                      'const struct v4l2_format_info *info=&p010;\n' + setup +
                      '\nreturn 0; }\n' + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(source), '-o', str(exe)], check=True)
    sei = subprocess.check_output([str(exe)])
    # Independent decoder checks emulation prevention, payload lengths, endian
    # order, zero CLL preservation and mastering luminance units.
    stream = temp / 'hdr.hevc'
    # CPU libx265 64x64 red, yuv420p10le, PQ/BT.2020. This is a parser
    # fixture, not evidence of the Wave521C firmware's output precision.
    stream.write_bytes(bytes.fromhex((root / 'hevc-main10-fixture.hex').read_text()) + sei)
    result = subprocess.run(['ffmpeg', '-v', 'verbose', '-f', 'hevc', '-i', str(stream),
                             '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr[-2000:]
    for field, value in [('general_profile_idc', 2),
                         ('bit_depth_luma_minus8', 2),
                         ('bit_depth_chroma_minus8', 2),
                         ('colour_primaries', 9),
                         ('transfer_characteristics', 16),
                         ('matrix_coefficients', 9),
                         ('display_primaries_x[0]', 13250),
                         ('white_point_y', 16450),
                         ('max_display_mastering_luminance', 10000000),
                         ('min_display_mastering_luminance', 1),
                         ('max_content_light_level', 0),
                         ('max_content_light_level', 1000),
                         ('max_pic_average_light_level', 400)]:
        assert re.search(re.escape(field) + r'\s+[01]+\s+=\s+' + str(value) +
                         r'\b', result.stderr), (field, result.stderr[-6000:])
print('P010 capacity/profile setup and independently parsed HDR10 SEI: PASS')
