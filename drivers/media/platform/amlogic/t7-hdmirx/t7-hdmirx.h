/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T7_HDMIRX_H
#define __T7_HDMIRX_H

struct device;
struct v4l2_dv_timings;
struct v4l2_fract;

/* Require two stable samples matching the receiver's selected RGB8 profile. */
int t7_hdmirx_check_video(struct device *dev);

/* One process-context sample, without the stable check's inter-sample delay. */
int t7_hdmirx_check_video_now(struct device *dev);

/* Allocation bound, independent of the currently detected input mode. */
int t7_hdmirx_get_max_size(struct device *dev, unsigned int *width, unsigned int *height);

/* Measured timing of a locked, supported RGB8 input; never a cached mode. */
int t7_hdmirx_query_timings(struct device *dev, struct v4l2_dv_timings *timings,
			  struct v4l2_fract *interval);

/* Hold the video PHY configuration until the consumer has stopped all DMA. */
int t7_hdmirx_stream_get(struct device *dev, unsigned int width, unsigned int height);
void t7_hdmirx_stream_put(struct device *dev);

#endif
