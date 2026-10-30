// SPDX-License-Identifier: GPL-2.0
/* Host ABI contract test: cc -Wall -Wextra -Werror test_abi.c -o /tmp/adla-abi */
#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "drv/uapi/linux/adlak_api.h"

static_assert(sizeof(struct adlak_caps_desc) == 60, "QUERYCAP layout");
static_assert(sizeof(struct adlak_buf_req) == 80, "REQBUF layout");
static_assert(sizeof(struct adlak_extern_buf_info) == 84, "ATTACH layout");
static_assert(sizeof(struct adlak_buf_flush) == 36, "cache sync layout");
static_assert(sizeof(struct adlak_network_desc) == 116, "network layout");
static_assert(sizeof(struct adlak_network_invoke_desc) == 28, "invoke layout");
static_assert(sizeof(struct adlak_get_stat_desc) == 88, "wait layout");
static_assert(sizeof(struct adlak_profile_cfg_desc) == 24, "profiling layout");
static_assert(sizeof(struct adlak_context_attribute) == 4, "context layout");
static_assert(offsetof(struct adlak_buf_req, ret_desc) == 32, "buffer return offset");
static_assert(ADLAK_IOCTL_QUERYCAP == 0xc03c5800, "SDK QUERYCAP command");

int main(void)
{
	return 0;
}
