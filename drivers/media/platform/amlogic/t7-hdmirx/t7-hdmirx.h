/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __T7_HDMIRX_H
#define __T7_HDMIRX_H

struct device;

/* Require two stable progressive 1080p60 samples on the bound receiver. */
int t7_hdmirx_check_1080p(struct device *dev);

#endif
