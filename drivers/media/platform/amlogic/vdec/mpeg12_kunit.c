// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>
#include <media/v4l2-mem2mem.h>

#include "mpeg12.h"

static void initial_field_pair_references(struct kunit *test)
{
	struct v4l2_ctrl_mpeg2_picture first = {
		.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_I,
		.forward_ref_ts = 100, .backward_ref_ts = 200,
	};
	struct v4l2_ctrl_mpeg2_picture second = {
		.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_P,
		.forward_ref_ts = U64_MAX,
	};
	u64 refs[2] = {};

	KUNIT_EXPECT_EQ(test, mpeg12_reference_timestamps(&second, &first, true, refs), 0);
	first.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_P;
	KUNIT_ASSERT_EQ(test, mpeg12_reference_timestamps(&second, &first, true, refs), 1);
	KUNIT_EXPECT_EQ(test, refs[0], 100);
	first.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_B;
	KUNIT_ASSERT_EQ(test, mpeg12_reference_timestamps(&second, &first, true, refs), 2);
	KUNIT_EXPECT_EQ(test, refs[0], 100);
	KUNIT_EXPECT_EQ(test, refs[1], 200);
	KUNIT_ASSERT_EQ(test, mpeg12_reference_timestamps(&second, &first, false, refs), 1);
	KUNIT_EXPECT_EQ(test, refs[0], U64_MAX);
}

static void unidirectional_b_references(struct kunit *test)
{
	struct v4l2_ctrl_mpeg2_picture pic = {
		.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_B,
		.forward_ref_ts = U64_MAX, .backward_ref_ts = 0,
	};
	u64 refs[2];
	u32 specs[2] = { ~0u, 0x030201 };
	u32 ref0, ref1;
	unsigned int count = mpeg12_reference_timestamps(&pic, NULL, false, refs);

	/* The opening B fields in sony-ct1/ct2 have only a backward anchor. */
	KUNIT_ASSERT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), 0);
	KUNIT_EXPECT_EQ(test, ref0, specs[1]);
	KUNIT_EXPECT_EQ(test, ref1, specs[1]);
	refs[0] = 0;
	refs[1] = U64_MAX;
	specs[0] = specs[1];
	specs[1] = ~0u;
	KUNIT_ASSERT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), 0);
	KUNIT_EXPECT_EQ(test, ref0, specs[0]);
	KUNIT_EXPECT_EQ(test, ref1, ~0u);
	/* Two declared anchors must retain their distinct prediction directions. */
	refs[0] = 1000;
	refs[1] = 2000;
	specs[0] = 0x030302;
	specs[1] = 0x050504;
	KUNIT_ASSERT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), 0);
	KUNIT_EXPECT_EQ(test, ref0, specs[0]);
	KUNIT_EXPECT_EQ(test, ref1, specs[1]);
	/* A declared reference that cannot be resolved must still fail. */
	refs[1] = 1000;
	specs[1] = ~0u;
	KUNIT_EXPECT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), -EINVAL);
	refs[0] = U64_MAX;
	refs[1] = U64_MAX;
	specs[0] = ~0u;
	KUNIT_EXPECT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), -EINVAL);
	KUNIT_EXPECT_EQ(test, mpeg12_reference_registers(refs, specs, 1,
							 &ref0, &ref1), -EINVAL);
}

static void sony_zero_timestamp_anchor(struct kunit *test)
{
	struct v4l2_ctrl_mpeg2_picture first = {
		.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_B,
		.picture_structure = V4L2_MPEG2_PIC_TOP_FIELD,
		.forward_ref_ts = U64_MAX, .backward_ref_ts = 0,
	};
	struct v4l2_ctrl_mpeg2_picture second = first;
	struct vb2_v4l2_buffer *anchor, *output, *unused;
	struct vb2_queue *queue;
	u64 refs[2];
	u32 specs[2] = { ~0u, 0x050504 };
	u32 ref0, ref1;
	unsigned int count;

	queue = kunit_kzalloc(test, sizeof(*queue), GFP_KERNEL);
	anchor = kunit_kzalloc(test, sizeof(*anchor), GFP_KERNEL);
	output = kunit_kzalloc(test, sizeof(*output), GFP_KERNEL);
	unused = kunit_kzalloc(test, sizeof(*unused), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, queue);
	KUNIT_ASSERT_NOT_NULL(test, anchor);
	KUNIT_ASSERT_NOT_NULL(test, output);
	KUNIT_ASSERT_NOT_NULL(test, unused);
	queue->max_num_buffers = 2;
	queue->bufs = kunit_kcalloc(test, 2, sizeof(*queue->bufs), GFP_KERNEL);
	queue->bufs_bitmap = kunit_kzalloc(test, sizeof(unsigned long), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, queue->bufs);
	KUNIT_ASSERT_NOT_NULL(test, queue->bufs_bitmap);
	queue->bufs[0] = &unused->vb2_buf;
	queue->bufs[1] = &anchor->vb2_buf;
	set_bit(0, queue->bufs_bitmap);
	set_bit(1, queue->bufs_bitmap);
	/* A never-decoded buffer also starts at zero; only copied timestamps count. */
	KUNIT_EXPECT_PTR_EQ(test, vb2_find_buffer(queue, 0), NULL);
	v4l2_m2m_buf_copy_metadata(output, anchor);
	KUNIT_ASSERT_PTR_EQ(test, vb2_find_buffer(queue, 0), &anchor->vb2_buf);

	/* r70 ts=1000 is a top/bottom B pair; ts=2000 is a frame B. */
	second.picture_structure = V4L2_MPEG2_PIC_BOTTOM_FIELD;
	count = mpeg12_reference_timestamps(&second, &first, true, refs);
	KUNIT_ASSERT_EQ(test, count, 2);
	KUNIT_EXPECT_EQ(test, refs[0], U64_MAX);
	KUNIT_EXPECT_EQ(test, refs[1], 0);
	KUNIT_ASSERT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), 0);
	KUNIT_EXPECT_EQ(test, ref0, specs[1]);
	KUNIT_EXPECT_EQ(test, ref1, specs[1]);
	second.picture_structure = V4L2_MPEG2_PIC_FRAME;
	count = mpeg12_reference_timestamps(&second, NULL, false, refs);
	KUNIT_ASSERT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), 0);
	KUNIT_EXPECT_EQ(test, ref0, specs[1]);
	KUNIT_EXPECT_EQ(test, ref1, specs[1]);
	/* Reusing the anchor for a later picture must invalidate lookup of zero. */
	output->vb2_buf.timestamp = 3000;
	v4l2_m2m_buf_copy_metadata(output, anchor);
	KUNIT_EXPECT_PTR_EQ(test, vb2_find_buffer(queue, refs[1]), NULL);
	specs[1] = ~0u;
	KUNIT_EXPECT_EQ(test, mpeg12_reference_registers(refs, specs, count,
							 &ref0, &ref1), -EINVAL);
}

static u32 header_bits(const u8 *data, unsigned int bit, unsigned int count)
{
	u32 value = 0;

	while (count--) {
		value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1);
		bit++;
	}
	return value;
}

/* Only return a complete extension, so failed serialization cannot overread. */
static const u8 *find_extension(const u8 *data, size_t len, u8 id, size_t needed)
{
	size_t i;

	for (i = 0; i + 4 + needed <= len; i++)
		if (!memcmp(data + i, "\0\0\1\xb5", 4) && data[i + 4] >> 4 == id)
			return data + i + 4;
	return NULL;
}

static void vbv_matrices_and_field_flags(struct kunit *test)
{
	struct v4l2_ctrl_mpeg2_sequence seq = {
		.horizontal_size = 1920, .vertical_size = 1080,
		.vbv_buffer_size = 0x12345, .profile_and_level_indication = 0x48,
		.chroma_format = 1,
	};
	struct v4l2_ctrl_mpeg2_picture pic = {
		.picture_coding_type = V4L2_MPEG2_PIC_CODING_TYPE_B,
		.picture_structure = V4L2_MPEG2_PIC_FRAME,
		.f_code = { { 1, 2 }, { 3, 4 } },
	};
	struct v4l2_ctrl_mpeg2_quantisation quant;
	u8 *matrix[] = {
		quant.intra_quantiser_matrix, quant.non_intra_quantiser_matrix,
		quant.chroma_intra_quantiser_matrix, quant.chroma_non_intra_quantiser_matrix,
	};
	u8 data[512];
	const u8 *ext;
	unsigned int i, j, bit, progressive, with_seq;
	u32 len, vbv;

	for (i = 0; i < ARRAY_SIZE(matrix); i++)
		for (j = 0; j < 64; j++)
			matrix[i][j] = ((i * 64 + j) * 13) % 255 + 1;
	for (progressive = 0; progressive < 2; progressive++) {
		seq.flags = progressive ? V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE : 0;
		pic.flags = progressive ? V4L2_MPEG2_PIC_FLAG_PROGRESSIVE : 0;
		for (with_seq = 0; with_seq < 2; with_seq++) {
			memset(data, 0xa5, sizeof(data));
			len = mpeg12_write_headers(data, &seq, &pic, &quant, with_seq);
			KUNIT_ASSERT_LE(test, len, sizeof(data));
			KUNIT_EXPECT_NULL(test, memchr_inv(data + len, 0xa5, sizeof(data) - len));
			if (with_seq) {
				KUNIT_ASSERT_GE(test, len, 12);
				KUNIT_EXPECT_MEMEQ(test, data, "\0\0\1\xb3", 4);
				KUNIT_EXPECT_EQ(test, header_bits(data + 4, 0, 12), 1920);
				KUNIT_EXPECT_EQ(test, header_bits(data + 4, 12, 12), 1080);
				vbv = header_bits(data + 4, 51, 10);
				ext = find_extension(data, len, 1, 6);
				KUNIT_ASSERT_NOT_NULL(test, ext);
				vbv |= header_bits(ext, 32, 8) << 10;
				KUNIT_EXPECT_EQ(test, vbv, seq.vbv_buffer_size);
			}
			ext = find_extension(data, len, 8, 5);
			KUNIT_ASSERT_NOT_NULL(test, ext);
			KUNIT_EXPECT_EQ(test, header_bits(ext, 31, 1), progressive);
			KUNIT_EXPECT_EQ(test, header_bits(ext, 32, 1), progressive);
			ext = find_extension(data, len, 3, 257);
			KUNIT_ASSERT_NOT_NULL(test, ext);
			bit = 4;
			for (i = 0; i < ARRAY_SIZE(matrix); i++) {
				KUNIT_EXPECT_EQ(test, header_bits(ext, bit++, 1), 1);
				for (j = 0; j < 64; j++, bit += 8)
					KUNIT_EXPECT_EQ(test, header_bits(ext, bit, 8),
							matrix[i][j]);
			}
		}
	}
}

static struct kunit_case mpeg12_test_cases[] = {
	KUNIT_CASE(initial_field_pair_references),
	KUNIT_CASE(unidirectional_b_references),
	KUNIT_CASE(sony_zero_timestamp_anchor),
	KUNIT_CASE(vbv_matrices_and_field_flags),
	{}
};

static struct kunit_suite mpeg12_test_suite = {
	.name = "amlogic-mpeg12-parameters",
	.test_cases = mpeg12_test_cases,
};

kunit_test_suite(mpeg12_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
