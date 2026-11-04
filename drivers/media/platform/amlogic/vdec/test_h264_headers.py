#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Decode actual C-generated NALs and check every output-capacity boundary."""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

HARNESS = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <linux/v4l2-controls.h>
typedef uint8_t u8;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
#define fls64(v) (64 - __builtin_clzll(v))
"""

EXAMPLE = r"""
int example(void *buffer, size_t capacity, int mode)
{
    struct v4l2_ctrl_h264_sps sps = {
        .profile_idc = 100, .level_idc = 30, .chroma_format_idc = 1,
        .log2_max_pic_order_cnt_lsb_minus4 = 2, .max_num_ref_frames = 4,
        .pic_width_in_mbs_minus1 = 39, .pic_height_in_map_units_minus1 = 22,
        .flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
                 V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
    };
    struct v4l2_ctrl_h264_pps pps = {
        .num_ref_idx_l0_default_active_minus1 = 2, .weighted_bipred_idc = 2,
        .pic_init_qp_minus26 = -4, .chroma_qp_index_offset = -2,
        .second_chroma_qp_index_offset = -2,
        .flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
                 V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
                 V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT |
                 V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE,
    };
    struct v4l2_ctrl_h264_scaling_matrix matrix = {};
    const u8 intra4[] = {6,13,20,28,13,20,28,32,20,28,32,37,28,32,37,42};
    const u8 inter4[] = {10,14,20,24,14,20,24,27,20,24,27,30,24,27,30,34};
    const u8 intra8[] = {
        6,10,13,16,18,23,25,27,10,11,16,18,23,25,27,29,
        13,16,18,23,25,27,29,31,16,18,23,25,27,29,31,33,
        18,23,25,27,29,31,33,36,23,25,27,29,31,33,36,38,
        25,27,29,31,33,36,38,40,27,29,31,33,36,38,40,42};
    const u8 inter8[] = {
        9,13,15,17,19,21,22,24,13,13,17,19,21,22,24,25,
        15,17,19,21,22,24,25,27,17,19,21,22,24,25,27,28,
        19,21,22,24,25,27,28,30,21,22,24,25,27,28,30,32,
        22,24,25,27,28,30,32,33,24,25,27,28,30,32,33,35};
    for (int i = 0; i < 6; i++)
        memcpy(matrix.scaling_list_4x4[i], i < 3 ? intra4 : inter4, 16);
    memcpy(matrix.scaling_list_8x8[0], intra8, 64);
    memcpy(matrix.scaling_list_8x8[1], inter8, 64);
    if (mode == 1) {
        sps.pic_order_cnt_type = 1;
        sps.offset_for_non_ref_pic = INT32_MIN;
        sps.offset_for_top_to_bottom_field = INT32_MAX;
        sps.num_ref_frames_in_pic_order_cnt_cycle = 255;
        for (int i = 0; i < 255; i++) sps.offset_for_ref_frame[i] = INT32_MIN;
    } else if (mode == 2) {
        matrix.scaling_list_4x4[0][0] = 0;
    } else if (mode == 3) {
        sps.constraint_set_flags = 0x25;
        for (int i = 0; i < 6; i++)
            for (int j = 0; j < 16; j++)
                matrix.scaling_list_4x4[i][j] = (i * 17 + j * 13) % 255 + 1;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 64; j++)
                matrix.scaling_list_8x8[i][j] = (i * 29 + j * 19) % 255 + 1;
    }
    return aml_h264_build_headers(buffer, capacity, &sps, &pps, &matrix);
}
"""


class Bits:
    def __init__(self, nal):
        rbsp = nal[1:].replace(b"\0\0\3", b"\0\0")
        self.bits = "".join(f"{b:08b}" for b in rbsp)
        self.pos = 0

    def read(self, count=1):
        value = self.bits[self.pos:self.pos + count]
        if len(value) != count:
            raise ValueError("truncated RBSP")
        self.pos += count
        return int(value, 2) if count else 0

    def ue(self):
        zeros = 0
        while not self.read():
            zeros += 1
        return (1 << zeros) - 1 + self.read(zeros)

    def se(self):
        code = self.ue()
        return (code + 1) // 2 if code & 1 else -(code // 2)

    def matrix(self, side):
        values = [0] * (side * side)
        last = 8
        for diagonal in range(side * 2 - 1):
            rows = list(range(max(0, diagonal - side + 1), min(side, diagonal + 1)))
            if not diagonal & 1:
                rows.reverse()
            for row in rows:
                last = (last + self.se()) % 256
                values[row * side + diagonal - row] = last
        return values


class HeaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="h264-header-test-")
        path = Path(cls.directory.name)
        source = "\n".join(line for line in (HERE / "h264_headers.c").read_text().splitlines()
                           if not line.startswith("#include"))
        (path / "test.c").write_text(HARNESS + source + EXAMPLE)
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Werror", "-O2",
                        str(path / "test.c"), "-o", str(path / "test.so")], check=True)
        cls.library = ctypes.CDLL(str(path / "test.so"))
        cls.library.example.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        cls.library.example.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def build(self, mode=0):
        buffer = ctypes.create_string_buffer(8192)
        size = self.library.example(buffer, len(buffer), mode)
        self.assertGreater(size, 0)
        return buffer.raw[:size]

    def test_matrix_scan_and_constraint_bit_order(self):
        data = self.build(3)
        sps, pps = re.split(b"\x00\x00\x00\x01", data)[1:]
        self.assertEqual(sps[2], 0xa4)
        reader = Bits(pps)
        self.assertEqual([reader.ue(), reader.ue(), reader.read(), reader.read()], [0, 0, 1, 0])
        self.assertEqual([reader.ue(), reader.ue(), reader.ue()], [0, 2, 0])
        self.assertEqual([reader.read(), reader.read(2)], [1, 2])
        self.assertEqual([reader.se(), reader.se(), reader.se()], [-4, 0, -2])
        self.assertEqual([reader.read() for _ in range(5)], [1, 0, 0, 1, 1])
        for i in range(6):
            self.assertEqual(reader.read(), 1)
            self.assertEqual(reader.matrix(4), [(i * 17 + j * 13) % 255 + 1 for j in range(16)])
        for i in range(2):
            self.assertEqual(reader.read(), 1)
            self.assertEqual(reader.matrix(8), [(i * 29 + j * 19) % 255 + 1 for j in range(64)])
        self.assertEqual(reader.se(), -2)
        self.assertEqual(reader.read(), 1)

    def test_signed_extremes_and_long_poc_cycle(self):
        data = self.build(1)
        self.assertIn(b"\0\0\3", data)
        sps = re.split(b"\x00\x00\x00\x01", data)[1]
        reader = Bits(sps)
        reader.read(24)
        self.assertEqual([reader.ue() for _ in range(4)], [0, 1, 0, 0])
        reader.read(2)
        self.assertEqual([reader.ue(), reader.ue(), reader.read()], [0, 1, 0])
        self.assertEqual([reader.se(), reader.se(), reader.ue()], [-2147483648, 2147483647, 255])
        self.assertEqual([reader.se() for _ in range(255)], [-2147483648] * 255)

    def test_capacity_and_emulation_prevention_boundaries(self):
        for mode in (0, 1, 3):
            length = len(self.build(mode))
            for capacity in range(length + 1):
                buffer = (ctypes.c_ubyte * (length + 2))(*([0xa5] * (length + 2)))
                ret = self.library.example(ctypes.byref(buffer, 1), capacity, mode)
                self.assertEqual(ret, length if capacity == length else -28)
                self.assertEqual(buffer[0], 0xa5)
                self.assertTrue(all(x == 0xa5 for x in buffer[capacity + 1:]))
        buffer = ctypes.create_string_buffer(8192)
        self.assertEqual(self.library.example(buffer, len(buffer), 2), -22)


if __name__ == "__main__":
    unittest.main()
