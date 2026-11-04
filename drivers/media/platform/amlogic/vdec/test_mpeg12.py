#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise MPEG-2 parameter headers and IRQ/close ownership using actual C."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_t7_lifetime import COMMON, function, run_c

HERE = Path(__file__).resolve().parent


class Bits:
    def __init__(self, data):
        self.data = ''.join(f'{x:08b}' for x in data)
        self.offset = 0

    def read(self, count):
        value = self.data[self.offset:self.offset + count]
        if len(value) != count:
            raise ValueError('truncated header')
        self.offset += count
        return int(value, 2)


class MPEG12Tests(unittest.TestCase):
    def test_codec_switch_discards_previous_picture_and_iq_mode(self):
        code = function(HERE / 'mpeg12.c', 'mpeg12_restore_hw')
        registers = sorted(set(re.findall(
            r'regmap_(?:write|update_bits)\(hw->map\[DOS_BUS\], (\w+)', code)))
        definitions = '\n'.join(f'#define {name} {i}' for i, name in enumerate(registers))
        run_c(COMMON + """
#include <linux/videodev2.h>
#define DOS_BUS 0
#define AML_FMT_DST 0
struct aml_vdec_hw { unsigned int *map[1]; };
struct aml_mpeg12_ctx { unsigned int wks_paddr, cc_paddr; };
struct aml_vdec_ctx {
    struct aml_vdec_hw *dev;
    struct aml_mpeg12_ctx *codec_priv;
    struct { unsigned int coded_width, coded_height; } pic_info;
    struct { unsigned int fourcc; } dec_fmt[1];
};
static struct aml_vdec_hw *vdec_get_hw(struct aml_vdec_hw *hw) { return hw; }
static void regmap_write(unsigned int *map, unsigned int reg, unsigned int value)
{ map[reg] = value; }
static void regmap_update_bits(unsigned int *map, unsigned int reg,
                               unsigned int mask, unsigned int value)
{ map[reg] = (map[reg] & ~mask) | (value & mask); }
""" + definitions + '\n' + code + """
int main(void)
{
    unsigned int regs[128];
    struct aml_vdec_hw hw = { .map = {regs} };
    struct aml_mpeg12_ctx m = { .wks_paddr = 0x12340000, .cc_paddr = 0x12400000 };
    struct aml_vdec_ctx ctx = { .dev = &hw, .codec_priv = &m,
        .pic_info = {1920, 1088}, .dec_fmt = {{V4L2_PIX_FMT_NV21M}} };
    for (int i = 0; i < 128; i++) regs[i] = 0xa5a5a5a5;
    mpeg12_restore_hw(&ctx);
    assert(regs[VCOP_CTRL_REG] == 0 && regs[MB_INFO] == 0);
    assert(regs[F_CODE_REG] == 0 && regs[SLICE_VER_POS_PIC_TYPE] == 0);
    assert(regs[MREG_CO_MV_START] == m.wks_paddr && regs[MREG_CC_ADDR] == m.cc_paddr);
    assert((regs[MDEC_PIC_DC_CTRL] & (3 << 16)) == (3 << 16));
    ctx.dec_fmt[0].fourcc = V4L2_PIX_FMT_NV12;
    mpeg12_restore_hw(&ctx);
    assert((regs[MDEC_PIC_DC_CTRL] & (3 << 16)) == (2 << 16));
    assert((regs[MDEC_PIC_DC_CTRL] & ~(3 << 16)) == (0xa5a5a5a5 & ~(3 << 16)));
    return 0;
}
""")

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='mpeg12-headers-')
        root = Path(cls.temp.name)
        text = (HERE / 'mpeg12.c').read_text()
        start = text.index('struct bitw {')
        structure = text[start:text.index('};', start) + 2]
        code = '\n'.join(function(HERE / 'mpeg12.c', name) for name in
                         ('put_bits', 'put_align', 'put_start', 'mpeg12_write_headers'))
        (root / 'headers.c').write_text(COMMON + """
#include <linux/v4l2-controls.h>
typedef uint8_t u8;
""" + structure + code + """
unsigned int build(unsigned char *out, unsigned int *v, unsigned char *q, int with_seq)
{
    struct v4l2_ctrl_mpeg2_sequence seq = {
        .horizontal_size = v[0], .vertical_size = v[1], .vbv_buffer_size = v[2],
        .profile_and_level_indication = v[3], .chroma_format = v[4], .flags = v[5]
    };
    struct v4l2_ctrl_mpeg2_picture pic = {
        .picture_coding_type = v[6], .picture_structure = v[7],
        .intra_dc_precision = v[8], .flags = v[9],
        .f_code = {{v[10], v[11]}, {v[12], v[13]}}
    };
    return mpeg12_write_headers(out, &seq, &pic,
           (struct v4l2_ctrl_mpeg2_quantisation *)q, with_seq);
}
""")
        subprocess.run(['cc', '-Wall', '-Werror', '-shared', '-fPIC',
                        str(root / 'headers.c'), '-o', str(root / 'headers.so')], check=True)
        cls.lib = ctypes.CDLL(str(root / 'headers.so'))
        cls.lib.build.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        cls.lib.build.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def headers(self, values, matrices, with_seq=True):
        output = (ctypes.c_ubyte * 1026)(*([0xa5] * 1026))
        params = (ctypes.c_uint * 14)(*values)
        quant = (ctypes.c_ubyte * 256)(*matrices)
        length = self.lib.build(ctypes.byref(output, 1), params, quant, with_seq)
        self.assertLess(length, 1024)
        self.assertEqual(output[0], 0xa5)
        self.assertTrue(all(x == 0xa5 for x in output[length + 1:]))
        return bytes(output[1:length + 1])

    def test_vbv_units_matrix_updates_and_interlaced_header(self):
        matrices = [(i * 13) % 255 + 1 for i in range(256)]
        for progressive in (False, True):
            values = [1920, 1080, 0x12345, 0x48, 1, int(progressive),
                      3, 3, 0, 0x80 if progressive else 0, 1, 2, 3, 4]
            for with_seq in (True, False):
                chunks = self.headers(values, matrices, with_seq).split(b'\0\0\1')[1:]
                if with_seq:
                    r = Bits(chunks[0][1:])
                    self.assertEqual((r.read(12), r.read(12)), (1920, 1080))
                    r.read(27)
                    vbv = r.read(10)
                    r = Bits(chunks[1][1:])
                    self.assertEqual(r.read(4), 1)
                    r.read(8 + 1 + 2 + 2 + 2 + 12 + 1)
                    self.assertEqual(vbv | r.read(8) << 10, 0x12345)
                extension = next(c for c in chunks if c[0] == 0xb5 and c[1] >> 4 == 8)
                r = Bits(extension[1:])
                r.read(4 + 16 + 2 + 2 + 7)
                self.assertEqual(r.read(1), int(progressive))  # chroma_420_type
                self.assertEqual(r.read(1), int(progressive))
                extension = next(c for c in chunks if c[0] == 0xb5 and c[1] >> 4 == 3)
                r = Bits(extension[1:])
                r.read(4)
                for i in range(4):
                    self.assertEqual(r.read(1), 1)
                    self.assertEqual([r.read(8) for _ in range(64)], matrices[i * 64:(i + 1) * 64])

    @unittest.skipUnless(shutil.which('ffmpeg'), 'FFmpeg is needed for pixel roundtrip')
    def test_rebuilt_ipb_headers_preserve_software_decoded_pixels(self):
        root = Path(self.temp.name)
        original, rebuilt = root / 'original.m2v', root / 'rebuilt.m2v'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                        'testsrc2=size=320x240:rate=25', '-frames:v', '16',
                        '-c:v', 'mpeg2video', '-threads', '1', '-q:v', '4', '-bf', '2',
                        '-intra_matrix', ','.join(str(8 + i % 17) for i in range(64)),
                        '-inter_matrix', ','.join(str(16 + i % 13) for i in range(64)),
                        '-f', 'mpeg2video', str(original)], check=True)
        values = [0] * 14
        matrices = [0] * 256
        stream = bytearray()
        slices = bytearray()
        types = set()

        def flush():
            if slices:
                stream.extend(self.headers(values, matrices))
                stream.extend(slices)
                slices.clear()

        for chunk in original.read_bytes().split(b'\0\0\1')[1:]:
            code = chunk[0]
            if code in (0, 0xb3, 0xb7, 0xb8):
                flush()
            r = Bits(chunk[1:])
            if code == 0xb3:
                values[0], values[1] = r.read(12), r.read(12)
                r.read(27)
                values[2] = r.read(10)
                r.read(1)
                self.assertEqual(r.read(1), 1)
                matrices[:64] = [r.read(8) for _ in range(64)]
                self.assertEqual(r.read(1), 1)
                matrices[64:128] = [r.read(8) for _ in range(64)]
                matrices[128:] = matrices[:128]
            elif code == 0:
                r.read(10)
                values[6] = r.read(3)
                types.add(values[6])
            elif code == 0xb5:
                extension = r.read(4)
                if extension == 1:
                    values[3] = r.read(8)
                    values[5] = r.read(1)
                    values[4] = r.read(2)
                    values[0] |= r.read(2) << 12
                    values[1] |= r.read(2) << 12
                    r.read(13)
                    values[2] |= r.read(8) << 10
                elif extension == 8:
                    values[10:14] = [r.read(4) for _ in range(4)]
                    values[8], values[7] = r.read(2), r.read(2)
                    values[9] = sum(r.read(1) << i for i in range(7))
                    r.read(1)
                    values[9] |= r.read(1) << 7
            elif 1 <= code <= 0xaf:
                slices.extend(b'\0\0\1' + chunk)
        flush()
        stream.extend(b'\0\0\1\xb7')
        rebuilt.write_bytes(stream)
        self.assertEqual(types, {1, 2, 3})
        decoded = [subprocess.check_output([
            'ffmpeg', '-v', 'error', '-i', str(path), '-threads', '1',
            '-pix_fmt', 'yuv420p', '-f', 'rawvideo', 'pipe:1']) for path in (original, rebuilt)]
        self.assertEqual(len(decoded[0]), 320 * 240 * 3 // 2 * 16)
        self.assertEqual(decoded[0], decoded[1])

    def test_irq_signals_result_without_returning_dma_or_buffers(self):
        code = function(HERE / 'mpeg12.c', 'mpeg12_job_done')
        run_c(COMMON + """
struct aml_vdec_ctx { int decode_result, int_cond, queue; };
static unsigned int wakeups;
static void wake_up_interruptible(int *queue) { wakeups++; }
""" + code + """
int main(void)
{
    struct aml_vdec_ctx ctx = {};
    mpeg12_job_done(&ctx, -EIO);
    assert(ctx.decode_result == -EIO && ctx.int_cond && wakeups == 1);
    mpeg12_job_done(&ctx, 0);
    assert(!ctx.decode_result && wakeups == 2);
    return 0;
}
""")

    def test_close_retains_all_dma_when_stop_cannot_be_proven(self):
        code = function(HERE / 'mpeg12.c', 'aml_mpeg12_exit')
        run_c(COMMON + """
#define MPEG12_WORKSPACE_SIZE 1
#define MPEG12_CCBUF_SIZE 2
struct aml_vdec_hw { bool faulted; void *dev; };
struct aml_mpeg12_ctx {
    void *stage_vaddr, *wks_vaddr, *cc_vaddr, *fld_buf;
    unsigned long stage_paddr, wks_paddr, cc_paddr, stage_size;
};
struct aml_vdec_ctx {
    struct aml_vdec_hw *dev;
    struct aml_mpeg12_ctx *codec_priv;
    bool dos_clk_en;
};
static int stop_error, stops, dma_frees, heap_frees, finishes;
static struct aml_vdec_hw *vdec_get_hw(struct aml_vdec_hw *h) { return h; }
static int aml_stop_vdec_hw(struct aml_vdec_hw *hw) { stops++; return stop_error; }
static void aml_vdec_fault(struct aml_vdec_ctx *ctx) { ctx->dev->faulted = true; }
static void aml_mpeg12_finish(void *p) { finishes++; }
static void dma_free_coherent(void *d, unsigned long s, void *p, unsigned long a) { dma_frees++; }
static void kvfree(void *p) { heap_frees++; }
static void kfree(void *p) { heap_frees++; }
""" + code + """
int main(void)
{
    struct aml_vdec_hw hw = {};
    struct aml_mpeg12_ctx m = { .stage_vaddr = &m };
    struct aml_vdec_ctx ctx = { &hw, &m, true };
    stop_error = -ETIMEDOUT;
    aml_mpeg12_exit(&ctx);
    assert(hw.faulted && ctx.codec_priv == &m && stops == 1);
    assert(!dma_frees && !heap_frees && !finishes);
    aml_mpeg12_exit(&ctx);
    assert(stops == 1 && !dma_frees);
    hw.faulted = false; stop_error = 0;
    aml_mpeg12_exit(&ctx);
    assert(!ctx.codec_priv && !ctx.dos_clk_en && finishes == 1);
    assert(dma_frees == 3 && heap_frees == 2);
    return 0;
}
""")


if __name__ == '__main__':
    unittest.main()
