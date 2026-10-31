#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check actual LUT CRC code against independent canonical zlib vectors."""
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
start = source.index('static u32 t7_vdin_lut_crc(')
function = source[start:source.index('\n}', start) + 2]
vectors = [([0x04030201, 0x08070605], 0), ([4, 8, 1020, 16380], 2), ([], 0)]
calls = []
for i, (values, shift) in enumerate(vectors):
    encoded = struct.pack('<' + 'I' * len(values), *(v >> shift for v in values))
    expected = zlib.crc32(encoded)
    calls.append('u32 v%d[] = {%s}; assert(t7_vdin_lut_crc(v%d, %d, %d) == %du);' %
                 (i, ','.join(map(str, values)) or '0', i, len(values), shift, expected))
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <zlib.h>
typedef uint32_t u32;
typedef uint32_t __le32;
typedef unsigned char u8;
#define cpu_to_le32(x) (x)
static u32 crc32_le(u32 seed, const u8 *bytes, unsigned count)
{ return crc32(seed ^ ~0U, bytes, count) ^ ~0U; }
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / 'test.c'
    c.write_text(fixture + function + '\nint main(void) {' + '\n'.join(calls) + '}\n')
    binary = Path(temp) / 'test'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-lz', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('HDR LUT LE port-sample CRC: PASS')
