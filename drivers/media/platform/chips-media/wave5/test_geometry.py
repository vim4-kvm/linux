#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check visible encoder geometry against the actual block crop calculation."""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


class Bits:
    def __init__(self, data):
        self.data = data
        self.position = 0

    def read(self, count):
        if count < 0 or self.position + count > len(self.data) * 8:
            raise ValueError('truncated SPS')
        value = 0
        for _ in range(count):
            value = value * 2 + ((self.data[self.position // 8] >>
                                  (7 - self.position % 8)) & 1)
            self.position += 1
        return value

    def ue(self):
        zeroes = 0
        while not self.read(1):
            zeroes += 1
            if zeroes > 31:
                raise ValueError('oversized Exp-Golomb code')
        return (1 << zeroes) - 1 + self.read(zeroes)


def nal_rbsp(data):
    """Remove emulation prevention, rejecting malformed escape sequences."""
    rbsp = bytearray()
    zeroes = 0
    for index, byte in enumerate(data):
        if zeroes == 2 and byte == 3:
            if index + 1 >= len(data) or data[index + 1] > 3:
                raise ValueError('invalid emulation prevention')
            zeroes = 0
            continue
        rbsp.append(byte)
        zeroes = min(2, zeroes + 1) if byte == 0 else 0
    return rbsp


def sei_messages(nal, hevc=True):
    """Decode Annex D HDR SEI payloads without trusting payload sizes.

    The caller supplies a complete AVC/HEVC NAL without its Annex-B start code.
    Unknown SEI messages are retained; their presence is not a parsing failure.
    """
    if not nal or nal[0] & 0x80:
        raise ValueError('invalid SEI NAL header')
    if hevc:
        if len(nal) < 3 or (nal[0] >> 1) & 63 not in (39, 40) or not nal[1] & 7:
            raise ValueError('invalid HEVC SEI NAL header')
    elif nal[0] & 31 != 6:
        raise ValueError('invalid AVC SEI NAL header')
    rbsp = nal_rbsp(nal[2 if hevc else 1:])
    offset = 0
    messages = []
    while offset < len(rbsp):
        if rbsp[offset:] == b'\x80':
            return messages
        values = []
        for _ in range(2):
            value = 0
            while offset < len(rbsp) and rbsp[offset] == 255:
                value += 255
                offset += 1
            if offset == len(rbsp):
                raise ValueError('truncated SEI type/size')
            value += rbsp[offset]
            offset += 1
            values.append(value)
        kind, size = values
        if offset + size >= len(rbsp):
            raise ValueError('truncated SEI payload or trailing bits')
        payload = rbsp[offset:offset + size]
        offset += size
        if kind == 137:
            if size != 24:
                raise ValueError('invalid mastering display SEI size')
            xy = struct.unpack('>8H2I', payload)
            decoded = {'primaries': tuple(zip(xy[:6:2], xy[1:6:2])),
                       'white_point': xy[6:8], 'max_luminance': xy[8],
                       'min_luminance': xy[9]}
        elif kind == 144:
            if size != 4:
                raise ValueError('invalid content light SEI size')
            cll, fall = struct.unpack('>2H', payload)
            decoded = {'max_content_light_level': cll, 'max_pic_average_light_level': fall}
        else:
            decoded = bytes(payload).hex()
        messages.append((kind, decoded))
    raise ValueError('missing SEI trailing bits')


def hevc_sps_geometry(nal):
    """Read SPS dimensions/crop; accept a complete NAL without a start code."""
    if len(nal) < 3 or nal[0] & 0x80 or (nal[0] >> 1) & 63 != 33 or not nal[1] & 7:
        raise ValueError('invalid SPS NAL header')
    bits = Bits(nal_rbsp(nal[2:]))
    bits.read(4)  # sps_video_parameter_set_id
    layers = bits.read(3)
    if layers > 6:
        raise ValueError('invalid sub-layer count')
    bits.read(1)  # sps_temporal_id_nesting_flag
    bits.read(96)  # general profile_tier_level, including level_idc
    present = [(bits.read(1), bits.read(1)) for _ in range(layers)]
    if layers:
        bits.read(2 * (8 - layers))
    for profile, level in present:
        if profile:
            bits.read(88)
        if level:
            bits.read(8)
    if bits.ue() > 15:
        raise ValueError('invalid SPS identifier')
    chroma = bits.ue()
    if chroma > 3:
        raise ValueError('invalid chroma format')
    separate = bits.read(1) if chroma == 3 else 0
    width, height = bits.ue(), bits.ue()
    crop = [bits.ue() for _ in range(4)] if bits.read(1) else [0] * 4
    sub_width = 2 if chroma in (1, 2) and not separate else 1
    sub_height = 2 if chroma == 1 and not separate else 1
    pixels = tuple(value * (sub_width if i < 2 else sub_height)
                   for i, value in enumerate(crop))
    visible = width - pixels[0] - pixels[1], height - pixels[2] - pixels[3]
    if min(visible) <= 0:
        raise ValueError('empty conformance window')
    return (width, height), pixels, visible


if len(sys.argv) == 3 and sys.argv[1] in ('--hevc-sei', '--avc-sei'):
    hevc = sys.argv[1] == '--hevc-sei'
    for nal in re.split(b'\x00\x00\x00?\x01', Path(sys.argv[2]).read_bytes()):
        if nal and ((nal[0] >> 1) & 63 in (39, 40) if hevc else nal[0] & 31 == 6):
            print(sei_messages(nal, hevc))
    raise SystemExit(0)

if len(sys.argv) == 2:
    # Offline inspection of a future hardware-produced Annex-B stream.
    nals = re.split(b'\x00\x00\x00?\x01', Path(sys.argv[1]).read_bytes())
    sps = [nal for nal in nals if nal and (nal[0] >> 1) & 63 == 33]
    if not sps:
        raise SystemExit('no HEVC SPS found')
    for nal in sps:
        print(hevc_sps_geometry(nal))
    raise SystemExit(0)

# Historical Wave521C NALs from earlier windows: parser regressions only,
# not evidence that this candidate has been run on hardware.
for encoded, dimensions in (
        ('42010101600000030000030000030000030096a002ac8030165ae49306b96640',
         (1368, 768)),
        ('42010101600000030000030000030000030096a001e020021c596b924c1ae599',
         (3840, 2160))):
    coded, offsets, visible = hevc_sps_geometry(bytes.fromhex(encoded))
    assert coded == visible == dimensions and offsets == (0, 0, 0, 0)
    for length in range(20):
        try:
            hevc_sps_geometry(bytes.fromhex(encoded)[:length])
        except ValueError:
            pass
        else:
            raise AssertionError('truncated SPS accepted')
print('Historical Wave521C SPS dimensions and truncated headers passed')

# Independent syntax vectors. These are not a claim that Wave521 emits HDR SEI.
mastering = struct.pack('>8H2I', 13250, 34500, 7500, 3000, 34000, 16000,
                        15635, 16450, 10000000, 50)
light = struct.pack('>2H', 1000, 400)
payload = bytes((137, 24)) + mastering + bytes((144, 4)) + light + b'\x80'
escaped = bytearray()
zeroes = 0
for byte in payload:
    if zeroes == 2 and byte <= 3:
        escaped.append(3)
        zeroes = 0
    escaped.append(byte)
    zeroes = min(2, zeroes + 1) if byte == 0 else 0
for header, hevc in ((b'\x4e\x01', True), (b'\x50\x01', True), (b'\x06', False)):
    nal = header + escaped
    messages = sei_messages(nal, hevc)
    assert messages[0][1]['white_point'] == (15635, 16450)
    assert messages[0][1]['min_luminance'] == 50
    assert messages[1] == (144, {'max_content_light_level': 1000,
                                 'max_pic_average_light_level': 400})
    for length in range(len(nal)):
        try:
            sei_messages(nal[:length], hevc)
        except ValueError:
            pass
        else:
            raise AssertionError('truncated SEI accepted')
    # Payload type/size extension, unknown messages, and embedded 00 00 01.
    assert sei_messages(header + b'\xff\x01\x03\x00\x00\x03\x01\x80', hevc) == [
        (256, '000001')]
    for invalid in (b'\x89\x17' + mastering + b'\x80', b'\x90\x03abc\x80',
                    b'\x00\x00\x03\x04\x80', b'\xff', b'\x00\xff', b'\x80\x00'):
        try:
            sei_messages(header + invalid, hevc)
        except ValueError:
            pass
        else:
            raise AssertionError('malformed SEI accepted')
print('AVC/HEVC HDR SEI sizes, values, escaping and truncation passed')

hardware = Path(__file__).with_name('wave5-hw.c').read_text()
encoder = Path(__file__).with_name('wave5-vpu-enc.c').read_text()
picture = encoder[encoder.index('static void wave5_set_enc_picture_size('):
                       encoder.index('static int wave5_set_vbv_size(')]
crop = hardware[hardware.index('static void wave5_set_enc_crop_info('):
                hardware.index('static u32 wave5_enc_sps_profile(')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define W_AVC_ENC 1
#define W_HEVC_ENC 2
#define ALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define WARN(condition, ...) assert(!(condition))
struct enc_wave_param {
    int conf_win_top, conf_win_bot, conf_win_left, conf_win_right;
};
struct enc_open_param { unsigned pic_width, pic_height; struct enc_wave_param wave_param; };
struct vpu_instance { int std; struct { int left, top; unsigned width, height; } conf_win; };
'''
checks = r'''
int main(void)
{
    const int modes[][2] = {
        {640,480}, {720,400}, {800,600}, {1024,768}, {1280,720},
        {1280,1024}, {1366,768}, {1440,900}, {1600,900}, {1680,1050},
        {1920,1080}, {1920,1200}, {2560,1440}, {3840,2160}
    };
    for (unsigned i=0; i<sizeof(modes)/sizeof(modes[0]); i++) {
        unsigned width=modes[i][0], height=modes[i][1];
        struct enc_wave_param p={0};
        wave5_set_enc_crop_info(W_AVC_ENC,&p,0,width,height);
        assert(ALIGN(width,16)-p.conf_win_left-p.conf_win_right==width);
        assert(ALIGN(height,16)-p.conf_win_top-p.conf_win_bot==height);
        assert(!(p.conf_win_right%2) && !(p.conf_win_bot%2));
        /* Rotation swaps axes, including the non-block-aligned crop. */
        p=(struct enc_wave_param){0};
        wave5_set_enc_crop_info(W_AVC_ENC,&p,3,width,height);
        assert(ALIGN(height,16)-p.conf_win_left-p.conf_win_right==height);
        assert(ALIGN(width,16)-p.conf_win_top-p.conf_win_bot==width);
        struct vpu_instance inst={.std=W_HEVC_ENC,.conf_win={0,0,width,height}};
        struct enc_open_param op={0};
        wave5_set_enc_picture_size(&op,&inst);
        assert(op.pic_width==ALIGN(width,8) && op.pic_height==ALIGN(height,8));
        /* This is the value written by SRC_SIZE and CONF_WIN, not a software
         * header rewrite. The firmware must emit the corresponding SPS. */
        wave5_set_enc_crop_info(W_HEVC_ENC,&op.wave_param,0,op.pic_width,op.pic_height);
        assert(op.pic_width-op.wave_param.conf_win_right==width);
        assert(op.pic_height-op.wave_param.conf_win_bot==height);
        printf("geometry %u %u %u %u %u %u %u %u\n", width, height,
               op.pic_width, op.pic_height, op.wave_param.conf_win_left,
               op.wave_param.conf_win_right, op.wave_param.conf_win_top,
               op.wave_param.conf_win_bot);
        for (unsigned rotation=0; rotation<4; rotation++) {
            op=(struct enc_open_param){0};
            inst.conf_win.left=2; inst.conf_win.top=4;
            wave5_set_enc_picture_size(&op,&inst);
            wave5_set_enc_crop_info(W_HEVC_ENC,&op.wave_param,rotation?1|(rotation<<1):0,
                                   op.pic_width,op.pic_height);
            unsigned coded_w=rotation?ALIGN(op.pic_width,32):op.pic_width;
            unsigned coded_h=rotation?ALIGN(op.pic_height,32):op.pic_height;
            if (rotation&1) { unsigned swap=coded_w; coded_w=coded_h; coded_h=swap; }
            assert(coded_w-op.wave_param.conf_win_left-op.wave_param.conf_win_right==
                   (rotation&1?height:width));
            assert(coded_h-op.wave_param.conf_win_top-op.wave_param.conf_win_bot==
                   (rotation&1?width:height));
            if (i==6 && rotation<2)
                printf("geometry %u %u %u %u %u %u %u %u\n",
                       rotation?height:width, rotation?width:height,
                       coded_w, coded_h, op.wave_param.conf_win_left,
                       op.wave_param.conf_win_right, op.wave_param.conf_win_top,
                       op.wave_param.conf_win_bot);
        }
    }
    puts("Wave5 AVC visible geometry, even crop units and rotated padding passed");
}
'''
with tempfile.TemporaryDirectory(prefix='wave5-geometry-') as directory:
    source = Path(directory) / 'geometry.c'
    executable = Path(directory) / 'geometry'
    source.write_text(prefix + crop + picture + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(executable)], check=True)
    result = subprocess.run([str(executable)], text=True, capture_output=True, check=True)
    geometries = {tuple(map(int, line.split()[1:]))
                  for line in result.stdout.splitlines() if line.startswith('geometry ')}
    print(result.stdout.splitlines()[-1])

# Complete SPS NALs generated by x265 (8-pixel minimum CU), then cropped by
# hevc_metadata and checked with ffprobe. These are independent syntax oracles,
# not Wave5 output. Match their geometry against the production C above.
references = (
    ((1368, 768), (0, 2, 0, 0), (1366, 768),
     '420101016000000300900000030000030078a002ac80301d796565b930bc05a02000000300200000030021'),
    ((1440, 904), (0, 0, 0, 4), (1440, 900),
     '420101016000000300900000030000030078a002d080389f796565b930bc05a02000000300200000030021'),
    ((1680, 1056), (0, 0, 0, 6), (1680, 1050),
     '420101016000000300900000030000030078a00348801087c996565b930bc05a020000030002000003000210'),
    ((1368, 776), (2, 0, 4, 4), (1366, 768),
     '420101016000000300900000030000030078a002ac80309ab796565b930bc05a020000030002000003000210'),
    ((800, 1376), (4, 28, 8, 2), (768, 1366),
     '420101016000000300900000030000030078a0064200561b1e5596565b930bc05a020000030002000003000210'),
)
for coded, offsets, visible, encoded in references:
    assert hevc_sps_geometry(bytes.fromhex(encoded)) == (coded, offsets, visible)
    assert visible + coded + offsets in geometries
print('Five independent SPS conformance windows match Wave5 command geometry')
