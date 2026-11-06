// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

/* Compiled beside the protocol implementation to exercise its private entry points. */
struct stateful_fixture {
	struct aml_vdec_stateful_ctx stateful;
	struct aml_vdec_node_desc node;
	struct v4l2_m2m_ctx m2m;
	struct video_device video;
	struct v4l2_m2m_buffer input;
	struct v4l2_m2m_buffer capture;
	struct aml_vdec_stream_info info;
	unsigned int frames;
	bool discard;
	bool event_before_completion;
};

static struct stateful_fixture *stateful_fixture(struct aml_vdec_ctx *ctx)
{
	return container_of(aml_vdec_stateful(ctx), struct stateful_fixture, stateful);
}

static int fixture_get_frame(struct aml_vdec_ctx *ctx, bool flush, struct aml_vdec_frame *frame)
{
	struct stateful_fixture *f = stateful_fixture(ctx);

	if (!f->frames)
		return 0;
	f->frames--;
	frame->buffer = &f->capture.vb;
	frame->disposition = f->discard ? AML_VDEC_FRAME_DISCARD : AML_VDEC_FRAME_OUTPUT;
	frame->final = !f->frames;
	return 1;
}

static void fixture_get_format(struct aml_vdec_ctx *ctx, struct aml_vdec_stream_info *info)
{
	*info = stateful_fixture(ctx)->info;
}

static const struct aml_vdec_stateful_ops fixture_codec_ops = {
	.get_frame = fixture_get_frame,
	.get_format = fixture_get_format,
};

static void fixture_capture_finish(void *priv)
{
	struct stateful_fixture *f = priv;

	/* vb2 calls this inside DONE, before the buffer enters its done list. */
	f->event_before_completion = v4l2_event_pending(&f->stateful.ctx.fh) > 0;
}

static const struct vb2_mem_ops fixture_memory_ops = {
	.finish = fixture_capture_finish,
};

static int stateful_test_init(struct kunit *test)
{
	struct stateful_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct v4l2_event_subscription change = { .type = V4L2_EVENT_SOURCE_CHANGE };
	struct v4l2_event_subscription eos = { .type = V4L2_EVENT_EOS };
	struct vb2_queue *capture;
	int ret;

	if (!f)
		return -ENOMEM;
	mutex_init(&f->stateful.lock);
	f->stateful.ctx.node = &f->node;
	f->stateful.ctx.m2m_ctx = &f->m2m;
	f->stateful.ctx.codec_priv = f;
	f->node.stateful_ops = &fixture_codec_ops;
	spin_lock_init(&f->video.fh_lock);
	INIT_LIST_HEAD(&f->video.fh_list);
	v4l2_fh_init(&f->stateful.ctx.fh, &f->video);
	f->stateful.ctx.fh.m2m_ctx = &f->m2m;
	ret = v4l2_event_subscribe(&f->stateful.ctx.fh, &change, 2, NULL);
	if (!ret)
		ret = v4l2_event_subscribe(&f->stateful.ctx.fh, &eos, 2, NULL);
	if (ret) {
		v4l2_fh_exit(&f->stateful.ctx.fh);
		return ret;
	}
	spin_lock_init(&f->m2m.out_q_ctx.rdy_spinlock);
	INIT_LIST_HEAD(&f->m2m.out_q_ctx.rdy_queue);
	f->m2m.out_q_ctx.q.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f->input.vb.vb2_buf.vb2_queue = &f->m2m.out_q_ctx.q;
	spin_lock_init(&f->m2m.out_q_ctx.q.done_lock);
	INIT_LIST_HEAD(&f->m2m.out_q_ctx.q.done_list);
	init_waitqueue_head(&f->m2m.out_q_ctx.q.done_wq);
	f->input.vb.vb2_buf.state = VB2_BUF_STATE_ACTIVE;
	atomic_set(&f->m2m.out_q_ctx.q.owned_by_drv_count, 1);
	spin_lock_init(&f->m2m.cap_q_ctx.rdy_spinlock);
	INIT_LIST_HEAD(&f->m2m.cap_q_ctx.rdy_queue);
	capture = &f->m2m.cap_q_ctx.q;
	capture->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	spin_lock_init(&capture->done_lock);
	INIT_LIST_HEAD(&capture->done_list);
	init_waitqueue_head(&capture->done_wq);
	capture->mem_ops = &fixture_memory_ops;
	atomic_set(&capture->owned_by_drv_count, 1);
	f->capture.vb.vb2_buf.vb2_queue = capture;
	f->capture.vb.vb2_buf.state = VB2_BUF_STATE_ACTIVE;
	f->capture.vb.vb2_buf.num_planes = 1;
	f->capture.vb.vb2_buf.synced = true;
	f->capture.vb.vb2_buf.planes[0].mem_priv = f;
	f->info.format.width = 448;
	f->info.format.height = 256;
	f->info.format.num_planes = 2;
	f->info.crop.width = 448;
	f->info.crop.height = 256;
	f->info.min_capture = 5;
	test->priv = f;
	return 0;
}

static void stateful_test_exit(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;

	v4l2_fh_exit(&f->stateful.ctx.fh);
}

static void initial_source_change_keeps_the_input_for_replay(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct v4l2_event event;

	v4l2_m2m_buf_queue(ctx->m2m_ctx, &f->input.vb);
	stateful_source_change(ctx);
	KUNIT_EXPECT_PTR_EQ(test, v4l2_m2m_next_src_buf(ctx->m2m_ctx), &f->input.vb);
	KUNIT_EXPECT_EQ(test, ctx->sequence_out, 0U);
	KUNIT_EXPECT_TRUE(test, f->stateful.wait_capture);
	KUNIT_EXPECT_EQ(test, f->stateful.min_capture, 5U);
	KUNIT_EXPECT_EQ(test, ctx->compose.width, 448U);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_SOURCE_CHANGE);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
}

static void resolution_change_event_precedes_last_without_stopping_output(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct v4l2_event event;

	f->stateful.format_known = true;
	ctx->is_cap_streamon = true;
	f->frames = 1;
	stateful_source_change(ctx);
	KUNIT_EXPECT_TRUE(test, f->event_before_completion);
	KUNIT_EXPECT_TRUE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_SOURCE_CHANGE);
}

static void stop_before_initial_format_completes_the_later_last_buffer(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct v4l2_decoder_cmd stop = { .cmd = V4L2_DEC_CMD_STOP };
	struct file file = { .private_data = &ctx->fh };
	struct v4l2_event event;

	KUNIT_ASSERT_EQ(test, stateful_buffered_decoder_cmd(&file, NULL, &stop), 0);
	KUNIT_EXPECT_TRUE(test, f->m2m.is_draining);
	KUNIT_EXPECT_TRUE(test, f->m2m.next_buf_last);
	KUNIT_EXPECT_EQ(test, v4l2_event_pending(&ctx->fh), 0);
	stateful_last_buffer_done(ctx, &f->capture.vb);
	KUNIT_EXPECT_TRUE(test, f->event_before_completion);
	KUNIT_EXPECT_TRUE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_TRUE(test, f->m2m.has_stopped);
	KUNIT_EXPECT_FALSE(test, f->m2m.is_draining);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_EOS);
}

static void discarded_picture_returns_to_ready_without_completion(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;

	f->frames = 1;
	f->discard = true;
	f->capture.vb.flags = V4L2_BUF_FLAG_LAST | V4L2_BUF_FLAG_KEYFRAME;
	KUNIT_EXPECT_FALSE(test, stateful_flush(ctx, VB2_BUF_STATE_DONE, true));
	KUNIT_EXPECT_PTR_EQ(test, v4l2_m2m_next_dst_buf(ctx->m2m_ctx), &f->capture.vb);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, f->stateful.sequence, 0U);
	KUNIT_EXPECT_EQ(test, f->capture.vb.flags, 0U);
	KUNIT_EXPECT_EQ(test, v4l2_event_pending(&ctx->fh), 0);
	KUNIT_EXPECT_TRUE(test, list_empty(&f->m2m.cap_q_ctx.q.done_list));
}

static void all_discarded_resolution_change_still_completes_empty_last(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;

	f->stateful.format_known = true;
	ctx->is_cap_streamon = true;
	f->frames = 1;
	f->discard = true;
	stateful_source_change(ctx);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, vb2_get_plane_payload(&f->capture.vb.vb2_buf, 0), 0UL);
	KUNIT_EXPECT_TRUE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_TRUE(test, f->event_before_completion);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
}

static void recoverable_input_error_keeps_queues_usable(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct aml_vdec_decode_result result = { .recoverable_error = true };
	struct v4l2_m2m_buffer *next = kunit_kzalloc(test, sizeof(*next), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, next);
	v4l2_m2m_buf_queue(ctx->m2m_ctx, &f->input.vb);
	stateful_consume_source(ctx, &result);
	KUNIT_EXPECT_EQ(test, f->input.vb.vb2_buf.state, VB2_BUF_STATE_ERROR);
	KUNIT_EXPECT_FALSE(test, f->m2m.out_q_ctx.q.error);
	KUNIT_EXPECT_FALSE(test, f->m2m.cap_q_ctx.q.error);
	KUNIT_EXPECT_FALSE(test, ctx->decode_failed);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
	next->vb.vb2_buf.vb2_queue = &f->m2m.out_q_ctx.q;
	next->vb.vb2_buf.state = VB2_BUF_STATE_ACTIVE;
	atomic_inc(&f->m2m.out_q_ctx.q.owned_by_drv_count);
	v4l2_m2m_buf_queue(ctx->m2m_ctx, &next->vb);
	f->frames = 1;
	result.recoverable_error = false;
	stateful_consume_source(ctx, &result);
	KUNIT_EXPECT_EQ(test, next->vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, ctx->sequence_out, 2U);
}

static void all_discarded_drain_completes_empty_last(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct aml_vdec_decode_result result = {};
	struct v4l2_event event;

	v4l2_m2m_buf_queue(ctx->m2m_ctx, &f->input.vb);
	f->m2m.is_draining = true;
	f->m2m.last_src_buf = &f->input.vb;
	f->frames = 1;
	f->discard = true;
	stateful_consume_source(ctx, &result);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, vb2_get_plane_payload(&f->capture.vb.vb2_buf, 0), 0UL);
	KUNIT_EXPECT_TRUE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_TRUE(test, f->m2m.has_stopped);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_EOS);
}

static void last_input_error_drains_prior_output_and_reports_eos(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct aml_vdec_decode_result result = { .recoverable_error = true };
	struct v4l2_event event;

	v4l2_m2m_buf_queue(ctx->m2m_ctx, &f->input.vb);
	f->m2m.is_draining = true;
	f->m2m.last_src_buf = &f->input.vb;
	f->frames = 1;
	stateful_consume_source(ctx, &result);
	KUNIT_EXPECT_EQ(test, f->input.vb.vb2_buf.state, VB2_BUF_STATE_ERROR);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_TRUE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_TRUE(test, f->m2m.has_stopped);
	KUNIT_EXPECT_FALSE(test, f->m2m.out_q_ctx.q.error);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_EOS);
}

static struct kunit_case stateful_test_cases[] = {
	KUNIT_CASE(initial_source_change_keeps_the_input_for_replay),
	KUNIT_CASE(resolution_change_event_precedes_last_without_stopping_output),
	KUNIT_CASE(stop_before_initial_format_completes_the_later_last_buffer),
	KUNIT_CASE(discarded_picture_returns_to_ready_without_completion),
	KUNIT_CASE(all_discarded_resolution_change_still_completes_empty_last),
	KUNIT_CASE(recoverable_input_error_keeps_queues_usable),
	KUNIT_CASE(last_input_error_drains_prior_output_and_reports_eos),
	KUNIT_CASE(all_discarded_drain_completes_empty_last),
	{}
};

static struct kunit_suite stateful_test_suite = {
	.name = "amlogic-vdec-stateful",
	.init = stateful_test_init,
	.exit = stateful_test_exit,
	.test_cases = stateful_test_cases,
};

kunit_test_suite(stateful_test_suite);
