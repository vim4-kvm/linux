/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ADLAK_KUNIT_H
#define ADLAK_KUNIT_H

#include <linux/types.h>

struct adlak_device;

#if IS_ENABLED(CONFIG_KUNIT)
int adlak_invoke_del_with_invokeid(struct adlak_device *padlak, s32 net_id, s32 invoke_id);
#endif

#endif
