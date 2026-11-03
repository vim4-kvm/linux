/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T7_HDMIRX_H
#define __T7_HDMIRX_H

struct device;
struct v4l2_dv_timings;
struct v4l2_fract;
struct v4l2_edid;
struct v4l2_pix_format;
struct v4l2_ctrl_hdr10_cll_info;
struct v4l2_ctrl_hdr10_mastering_display;

/* Fresh, checksum-qualified HDMI DRM metadata. Zero fields are preserved;
 * ENODATA means no current PQ packet, not a fabricated mastering default.
 */
int t7_hdmirx_get_hdr10(struct device *dev, struct v4l2_ctrl_hdr10_cll_info *cll,
			struct v4l2_ctrl_hdr10_mastering_display *mastering);

/* EDID changes are idempotent and refused while a capture lease owns DMA. */
int t7_hdmirx_get_edid(struct device *dev, struct v4l2_edid *edid);
int t7_hdmirx_set_edid(struct device *dev, const struct v4l2_edid *edid);

/* Require two stable samples matching the receiver's selected RGB8 profile. */
int t7_hdmirx_check_video(struct device *dev);

/* One process-context sample, without the stable check's inter-sample delay. */
int t7_hdmirx_check_video_now(struct device *dev);

/* Same admission, with a static reason string for bounded capture diagnostics. */
int t7_hdmirx_check_capture(struct device *dev, const char **reason);

/* Allocation bound, independent of the currently detected input mode. */
int t7_hdmirx_get_max_size(struct device *dev, unsigned int *width, unsigned int *height);

/* Measured timing of a locked, supported RGB8 input; never a cached mode. */
int t7_hdmirx_query_timings(struct device *dev, struct v4l2_dv_timings *timings,
			  struct v4l2_fract *interval);

/* Hold the video PHY configuration until the consumer has stopped all DMA. */
int t7_hdmirx_stream_get(struct device *dev, unsigned int width, unsigned int height);
void t7_hdmirx_stream_put(struct device *dev);

/* Stable input classification for the held stream lease, until stream_put. */
bool t7_hdmirx_stream_is_hdr(struct device *dev);
/* Qualified current input, or the immutable classification of an active lease. */
bool t7_hdmirx_capture_is_hdr(struct device *dev);
bool t7_hdmirx_stream_is_hlg(struct device *dev);
bool t7_hdmirx_stream_is_yuv(struct device *dev);
bool t7_hdmirx_stream_needs_yuv444_swap(struct device *dev);
bool t7_hdmirx_stream_is_full_range(struct device *dev);
bool t7_hdmirx_stream_is_bt601(struct device *dev);

/* Qualify native SDR YCbCr444/8 and report its colorimetry. */
int t7_hdmirx_yuv444_format(struct device *dev, struct v4l2_pix_format *pix);

#endif
