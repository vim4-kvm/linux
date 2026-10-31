#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Parse real AVC/HEVC SPS fixtures containing the actual driver's VUI bytes.

Fixtures are CPU-encoded 64x64 red frames, with encoder SEI removed.
This proves serialization, not that T7 firmware accepts the RBSP command.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent


def trace(path, codec):
    result = subprocess.run(['ffmpeg', '-v', 'verbose', '-f', codec, '-i', str(path),
                             '-c', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr[-2000:]
    return result.stderr.split('Sequence Parameter Set', 1)[1].split('Picture Parameter Set', 1)[0]


def position(text, field):
    return int(re.search(r'\]\s+(\d+)\s+' + field + r'\s', text)[1])


def unescape(data):
    return re.sub(b'\x00\x00\x03', b'\x00\x00', data)


def escape(data):
    out = bytearray()
    zeroes = 0
    for byte in data:
        if zeroes >= 2 and byte <= 3:
            out.append(3)
            zeroes = 0
        out.append(byte)
        zeroes = zeroes + 1 if byte == 0 else 0
    return bytes(out)


with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    c, exe = temp / 'vui.c', temp / 'vui'
    c.write_text(r'''
#include <time.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef uint8_t u8;
typedef uint32_t u32;
#include "wave5-vui.h"
int main(int argc, char **argv) {
 u8 data[5]; (void)argc;
 assert(wave5_make_vui(data,true,0,0,0,0)==39);
 assert(data[1]==1 && data[2]==1 && data[3]==1 && !(data[0]&2));
 assert(wave5_make_vui(data,true,V4L2_COLORSPACE_REC709,999,0,0)==-EINVAL);
 assert(wave5_make_vui(data,true,V4L2_COLORSPACE_REC709,0,999,0)==-EINVAL);
 assert(wave5_make_vui(data,true,V4L2_COLORSPACE_REC709,0,0,999)==-EINVAL);
 int mode=atoi(argv[2]);
 int bits=wave5_make_vui(data,atoi(argv[1]),mode==2?V4L2_COLORSPACE_BT2020:V4L2_COLORSPACE_REC709,
  mode==2?V4L2_XFER_FUNC_SMPTE2084:mode==1?V4L2_XFER_FUNC_SRGB:V4L2_XFER_FUNC_709,
  mode==2?V4L2_YCBCR_ENC_BT2020:V4L2_YCBCR_ENC_709,
  mode==2?V4L2_QUANTIZATION_FULL_RANGE:V4L2_QUANTIZATION_LIM_RANGE);
 if(bits<0) return 2;
 printf("%d ",bits); for(int i=0;i<5;i++) printf("%02x",data[i]);
}
''')
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(root), str(c),
                    '-o', str(exe)], check=True)
    for codec, fixture, hevc in [('hevc', 'hevc', 1), ('h264', 'avc', 0)]:
        raw = bytes.fromhex((root / (fixture + '-sps-fixture.hex')).read_text())
        baseline = temp / ('baseline.' + codec)
        baseline.write_bytes(raw)
        original = trace(baseline, codec)
        begin = position(original, 'vui_parameters_present_flag') + 1
        end = position(original, 'sps_extension_present_flag' if hevc else 'rbsp_stop_one_bit')
        for mode, expected in [(0, (1, 1, 1, 0)), (1, (1, 13, 1, 0)), (2, (9, 16, 9, 1))]:
            count, hexdata = subprocess.check_output([str(exe), str(hevc), str(mode)], text=True).split()
            payload = ''.join(f'{v:08b}' for v in bytes.fromhex(hexdata))[:int(count)]
            nals = [n for n in re.split(b'\x00\x00\x00?\x01', raw) if n]
            output = []
            for nal in nals:
                is_sps = ((nal[0] >> 1) & 63) == 33 if hevc else (nal[0] & 31) == 7
                if is_sps:
                    bits = ''.join(f'{v:08b}' for v in unescape(nal))
                    # Preserve SPS extensions, replacing its old trailing alignment.
                    tail = bits[end:bits.rfind('1') + 1]
                    bits = bits[:begin] + payload + tail
                    bits += '0' * (-len(bits) % 8)
                    nal = escape(bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)))
                output.append(b'\0\0\0\1' + nal)
            path = temp / ('case.' + codec)
            path.write_bytes(b''.join(output))
            parsed = trace(path, codec)
            for name, value in zip(['colour_primaries', 'transfer_characteristics',
                                    'matrix_coefficients', 'video_full_range_flag'], expected):
                assert re.search(name + r'\s+[01]+\s+=\s+' + str(value) + r'\b', parsed), parsed
print('Actual driver VUI in AVC/HEVC SPS: 709, sRGB, PQ/full parsed by FFmpeg: PASS')
