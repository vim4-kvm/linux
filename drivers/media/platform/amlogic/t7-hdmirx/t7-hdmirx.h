/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T7_HDMIRX_H
#define __T7_HDMIRX_H

struct device;

/* Require two stable samples matching the receiver's selected RGB8 profile. */
int t7_hdmirx_check_video(struct device *dev);

/* One process-context sample, without the stable check's inter-sample delay. */
int t7_hdmirx_check_video_now(struct device *dev);

/* Profile dimensions are immutable for the lifetime of the bound receiver. */
int t7_hdmirx_get_size(struct device *dev, unsigned int *width, unsigned int *height);

/* Hold the video PHY configuration until the consumer has stopped all DMA. */
int t7_hdmirx_stream_get(struct device *dev);
void t7_hdmirx_stream_put(struct device *dev);

#endif
