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
	struct v4l2_m2m_buffer second_capture;
	struct aml_vdec_stream_info info;
	unsigned int frames;
	unsigned int configurations;
	bool discard;
	bool configured;
	bool prepared_with_pending;
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
	frame->buffer = f->frames == 2 ? &f->second_capture.vb : &f->capture.vb;
	f->frames--;
	frame->disposition = f->discard ? AML_VDEC_FRAME_DISCARD : AML_VDEC_FRAME_OUTPUT;
	frame->final = !f->frames;
	return 1;
}

static void fixture_get_format(struct aml_vdec_ctx *ctx, struct aml_vdec_stream_info *info)
{
	*info = stateful_fixture(ctx)->info;
	/* Like HEVC, invalidate private storage until the next prepare. */
	stateful_fixture(ctx)->configured = false;
}

static int fixture_prepare(struct aml_vdec_ctx *ctx)
{
	struct stateful_fixture *f = stateful_fixture(ctx);

	f->prepared_with_pending = !!f->frames;
	if (!f->configured) {
		f->configurations++;
		f->configured = true;
	}
	return 0;
}

static const struct aml_vdec_stateful_ops fixture_codec_ops = {
	.prepare = fixture_prepare,
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

static void fixture_init_capture(struct stateful_fixture *f, struct vb2_v4l2_buffer *vb)
{
	vb->vb2_buf.vb2_queue = &f->m2m.cap_q_ctx.q;
	vb->vb2_buf.state = VB2_BUF_STATE_ACTIVE;
	vb->vb2_buf.num_planes = 1;
	vb->vb2_buf.synced = true;
	vb->vb2_buf.planes[0].mem_priv = f;
}

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
	fixture_init_capture(f, &f->capture.vb);
	fixture_init_capture(f, &f->second_capture.vb);
	f->info.format.width = 448;
	f->info.format.height = 256;
	f->info.format.pixelformat = V4L2_PIX_FMT_NV12M;
	f->info.format.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	f->info.format.num_planes = 2;
	f->info.format.plane_fmt[0].bytesperline = 448;
	f->info.format.plane_fmt[0].sizeimage = 131072;
	f->info.format.plane_fmt[1].bytesperline = 448;
	f->info.format.plane_fmt[1].sizeimage = 65536;
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
	stateful_config_change(ctx);
	KUNIT_EXPECT_PTR_EQ(test, v4l2_m2m_next_src_buf(ctx->m2m_ctx), &f->input.vb);
	KUNIT_EXPECT_EQ(test, ctx->sequence_out, 0U);
	KUNIT_EXPECT_TRUE(test, f->stateful.wait_capture);
	KUNIT_EXPECT_EQ(test, f->stateful.min_capture, 5U);
	KUNIT_EXPECT_EQ(test, ctx->compose.width, 448U);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_SOURCE_CHANGE);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
}

static void fixture_establish_format(struct stateful_fixture *f)
{
	f->stateful.format_known = true;
	f->stateful.ctx.is_cap_streamon = true;
	f->stateful.format = f->info.format;
	f->stateful.crop = f->info.crop;
	f->stateful.min_capture = f->info.min_capture;
	f->configured = true;
}

static void unchanged_format_flushes_without_last_and_keeps_input(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct vb2_buffer *first, *last;

	f->info.crop.width = 416;
	f->info.crop.height = 240;
	fixture_establish_format(f);
	f->stateful.sequence = 7;
	f->frames = 2;
	atomic_inc(&f->m2m.cap_q_ctx.q.owned_by_drv_count);
	v4l2_m2m_buf_queue(ctx->m2m_ctx, &f->input.vb);
	stateful_config_change(ctx);
	KUNIT_EXPECT_EQ(test, f->frames, 0U);
	KUNIT_EXPECT_FALSE(test, f->configured);
	KUNIT_EXPECT_EQ(test, f->configurations, 0U);
	KUNIT_ASSERT_FALSE(test, list_empty(&f->m2m.cap_q_ctx.q.done_list));
	first = list_first_entry(&f->m2m.cap_q_ctx.q.done_list, struct vb2_buffer, done_entry);
	last = list_last_entry(&f->m2m.cap_q_ctx.q.done_list, struct vb2_buffer, done_entry);
	KUNIT_EXPECT_PTR_EQ(test, first, &f->second_capture.vb.vb2_buf);
	KUNIT_EXPECT_PTR_EQ(test, last, &f->capture.vb.vb2_buf);
	KUNIT_EXPECT_EQ(test, first->state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, last->state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_EQ(test, f->second_capture.vb.sequence, 7U);
	KUNIT_EXPECT_EQ(test, f->capture.vb.sequence, 8U);
	KUNIT_EXPECT_FALSE(test, f->second_capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_FALSE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_EQ(test, v4l2_event_pending(&ctx->fh), 0);
	KUNIT_EXPECT_FALSE(test, f->stateful.wait_capture);
	KUNIT_EXPECT_TRUE(test, ctx->is_cap_streamon);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
	KUNIT_EXPECT_PTR_EQ(test, v4l2_m2m_next_src_buf(ctx->m2m_ctx), &f->input.vb);
	KUNIT_EXPECT_EQ(test, f->input.vb.vb2_buf.state, VB2_BUF_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, ctx->sequence_out, 0U);
	KUNIT_ASSERT_EQ(test, stateful_prepare_job(ctx), 0);
	KUNIT_EXPECT_EQ(test, f->configurations, 1U);
	KUNIT_EXPECT_FALSE(test, f->prepared_with_pending);
}

static const struct changed_query {
	const char *name;
} changed_queries[] = {
	{ "format_width" }, { "format_quantization" }, { "crop" }, { "min_capture" },
};

KUNIT_ARRAY_PARAM_DESC(changed_query, changed_queries, name);

static void changed_query_preserves_source_change_protocol(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	const struct changed_query *param = test->param_value;
	const char *query = param->name;
	struct v4l2_event event;

	fixture_establish_format(f);
	if (!strcmp(query, "format_width"))
		f->info.format.width += 64;
	else if (!strcmp(query, "format_quantization"))
		f->info.format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else if (!strcmp(query, "crop"))
		f->info.crop.width -= 16;
	else
		f->info.min_capture++;
	f->frames = 1;
	v4l2_m2m_buf_queue(ctx->m2m_ctx, &f->input.vb);
	stateful_config_change(ctx);
	KUNIT_EXPECT_TRUE(test, f->event_before_completion);
	KUNIT_EXPECT_TRUE(test, f->capture.vb.flags & V4L2_BUF_FLAG_LAST);
	KUNIT_EXPECT_EQ(test, f->capture.vb.vb2_buf.state, VB2_BUF_STATE_DONE);
	KUNIT_EXPECT_TRUE(test, f->stateful.wait_capture);
	KUNIT_EXPECT_FALSE(test, f->m2m.has_stopped);
	KUNIT_EXPECT_EQ(test, f->stateful.format.width, f->info.format.width);
	KUNIT_EXPECT_EQ(test, f->stateful.format.quantization, f->info.format.quantization);
	KUNIT_EXPECT_EQ(test, f->stateful.crop.width, f->info.crop.width);
	KUNIT_EXPECT_EQ(test, f->stateful.min_capture, f->info.min_capture);
	KUNIT_EXPECT_PTR_EQ(test, v4l2_m2m_next_src_buf(ctx->m2m_ctx), &f->input.vb);
	KUNIT_EXPECT_EQ(test, ctx->sequence_out, 0U);
	KUNIT_ASSERT_EQ(test, v4l2_event_dequeue(&ctx->fh, &event, true), 0);
	KUNIT_EXPECT_EQ(test, event.type, (u32)V4L2_EVENT_SOURCE_CHANGE);
}

static void resolution_change_event_precedes_last_without_stopping_output(struct kunit *test)
{
	struct stateful_fixture *f = test->priv;
	struct aml_vdec_ctx *ctx = &f->stateful.ctx;
	struct v4l2_event event;

	f->stateful.format_known = true;
	ctx->is_cap_streamon = true;
	f->frames = 1;
	stateful_config_change(ctx);
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
	stateful_config_change(ctx);
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
	KUNIT_CASE(unchanged_format_flushes_without_last_and_keeps_input),
	KUNIT_CASE_PARAM(changed_query_preserves_source_change_protocol,
			 changed_query_gen_params),
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
