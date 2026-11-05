// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/module.h>

#include "brcmu_wifi.h"
#include "cfg80211.h"
#include "common.h"
#include "feature.h"
#include "fwsignal.h"
#include "wcc/kunit.h"

static void brcmf_scan_conversion_test(struct kunit *test)
{
	const size_t max_tail = 64 * sizeof(__le16) + 2 * sizeof(struct brcmf_ssid_le);
	struct brcmf_scan_params_v2_le *src;
	struct brcmf_scan_params_le *dst;
	size_t src_size = offsetof(typeof(*src), channel_list) + max_tail;
	size_t dst_size = offsetof(typeof(*dst), channel_list) + max_tail + 1;
	unsigned int channels, ssids, i;

	src = kunit_kzalloc(test, src_size, GFP_KERNEL);
	dst = kunit_kmalloc(test, dst_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	for (channels = 0; channels <= 64; channels++) {
		for (ssids = 0; ssids <= 2; ssids++) {
			struct brcmf_ssid_le *ssid_list;
			size_t tail = channels * sizeof(__le16);

			memset(src, 0, src_size);
			memset(dst, 0xa5, dst_size);
			src->ssid_le.SSID_len = cpu_to_le32(7);
			memcpy(src->ssid_le.SSID, "DIRECT-", 7);
			eth_broadcast_addr(src->bssid);
			src->bss_type = DOT11_BSSTYPE_ANY;
			src->scan_type = cpu_to_le32(BRCMF_SCANTYPE_ACTIVE);
			src->nprobes = cpu_to_le32(3);
			src->active_time = cpu_to_le32(80);
			src->passive_time = cpu_to_le32(-1);
			src->home_time = cpu_to_le32(60);
			src->channel_num = cpu_to_le32(channels |
						     (ssids << BRCMF_SCAN_PARAMS_NSSID_SHIFT));
			for (i = 0; i < channels; i++)
				src->channel_list[i] = cpu_to_le16(0x1000 + i);
			if (ssids) {
				tail = roundup(tail, sizeof(u32));
				ssid_list = (void *)((u8 *)src->channel_list + tail);
				for (i = 0; i < ssids; i++) {
					ssid_list[i].SSID_len = cpu_to_le32(4);
					memset(ssid_list[i].SSID, 'a' + i, 4);
				}
				tail += ssids * sizeof(*ssid_list);
			}

			brcmf_scan_params_v2_to_v1(src, dst);
			KUNIT_EXPECT_MEMEQ(test, &dst->ssid_le, &src->ssid_le,
					   sizeof(src->ssid_le));
			KUNIT_EXPECT_MEMEQ(test, dst->bssid, src->bssid, ETH_ALEN);
			KUNIT_EXPECT_EQ(test, dst->bss_type, src->bss_type);
			KUNIT_EXPECT_EQ(test, dst->scan_type, BRCMF_SCANTYPE_ACTIVE);
			KUNIT_EXPECT_EQ(test, le32_to_cpu(dst->nprobes), le32_to_cpu(src->nprobes));
			KUNIT_EXPECT_EQ(test, le32_to_cpu(dst->active_time),
					le32_to_cpu(src->active_time));
			KUNIT_EXPECT_EQ(test, le32_to_cpu(dst->passive_time),
					le32_to_cpu(src->passive_time));
			KUNIT_EXPECT_EQ(test, le32_to_cpu(dst->home_time),
					le32_to_cpu(src->home_time));
			KUNIT_EXPECT_EQ(test, le32_to_cpu(dst->channel_num),
					le32_to_cpu(src->channel_num));
			KUNIT_EXPECT_MEMEQ(test, dst->channel_list, src->channel_list, tail);
			KUNIT_EXPECT_EQ(test, ((u8 *)dst->channel_list)[tail], (u8)0xa5);
		}
	}
}

static void brcmf_capability_policy_test(struct kunit *test)
{
	static const struct {
		int mode;
		bool discovery_bss;
		const char *caps;
		int err;
		bool disable_sae;
		int expected_mode;
		bool expected_sae;
	} cases[] = {
		{ BRCMF_FWS_FCMODE_AUTO, true, "proptxstatus extsae ", 0, false,
		  BRCMF_FWS_FCMODE_EXPLICIT_CREDIT, true },
		{ BRCMF_FWS_FCMODE_AUTO, false, "proptxstatus extsae ", 0, false,
		  BRCMF_FWS_FCMODE_AUTO, true },
		{ BRCMF_FWS_FCMODE_NONE, true, "proptxstatus extsae ", 0, false,
		  BRCMF_FWS_FCMODE_NONE, true },
		{ BRCMF_FWS_FCMODE_IMPLIED_CREDIT, true, "proptxstatus ", 0, false,
		  BRCMF_FWS_FCMODE_IMPLIED_CREDIT, false },
		{ BRCMF_FWS_FCMODE_EXPLICIT_CREDIT, true, "proptxstatus ", 0, false,
		  BRCMF_FWS_FCMODE_EXPLICIT_CREDIT, false },
		{ BRCMF_FWS_FCMODE_AUTO, true, "p2p extsae ", 0, false,
		  BRCMF_FWS_FCMODE_AUTO, true },
		{ BRCMF_FWS_FCMODE_AUTO, true, "proptxstatus extsae ", -EIO, false,
		  BRCMF_FWS_FCMODE_AUTO, false },
		{ BRCMF_FWS_FCMODE_AUTO, true, "proptxstatus extsae ", 0, true,
		  BRCMF_FWS_FCMODE_EXPLICIT_CREDIT, false },
	};
	struct brcmf_mp_device *settings;
	struct brcmf_pub *drvr;
	unsigned int i;

	drvr = kunit_kzalloc(test, sizeof(*drvr), GFP_KERNEL);
	settings = kunit_kzalloc(test, sizeof(*settings), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, drvr);
	KUNIT_ASSERT_NOT_NULL(test, settings);
	drvr->settings = settings;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		settings->fcmode = cases[i].mode;
		settings->feature_disable = 0;
		if (cases[i].disable_sae)
			settings->feature_disable = BIT(BRCMF_FEAT_SAE_EXT);
		drvr->feat_flags = BIT(BRCMF_FEAT_SCAN_V2);
		brcmf_wcc_feat_update(drvr, cases[i].caps, cases[i].err, cases[i].discovery_bss);
		KUNIT_EXPECT_EQ(test, settings->fcmode, cases[i].expected_mode);
		KUNIT_EXPECT_EQ(test, !!(drvr->feat_flags & BIT(BRCMF_FEAT_SAE_EXT)),
				cases[i].expected_sae);
		KUNIT_EXPECT_TRUE(test, drvr->feat_flags & BIT(BRCMF_FEAT_SCAN_V2));
	}
}

static struct kunit_case brcmf_p2p_cases[] = {
	KUNIT_CASE(brcmf_scan_conversion_test),
	KUNIT_CASE(brcmf_capability_policy_test),
	{}
};

static struct kunit_suite brcmf_p2p_suite = {
	.name = "brcmfmac-p2p-params",
	.test_cases = brcmf_p2p_cases,
};

kunit_test_suite(brcmf_p2p_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Broadcom FullMAC scan conversion and capability policy tests");
