// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2022 Broadcom Corporation
 */
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/etherdevice.h>
#include <brcm_hw_ids.h>
#include <core.h>
#include <bus.h>
#include <common.h>
#include <feature.h>
#include <fwvid.h>
#include <cfg80211.h>
#include <fwil.h>
#include <fwil_types.h>
#include <fweh.h>

#include "vops.h"

#define BRCMF_WCC_E_LAST		213
#define BRCMF_WCC_ASSOC_PAUSE	0
#define BRCMF_WCC_ASSOC_ABORT	1
#define BRCMF_WCC_ASSOC_SEND_AUTH	3

/* BCM43752 extsae uses the Broadcom association manager protocol, distinct
 * from the Infineon auth_status/mgmt_frame protocol despite the shared SAE
 * capability. Keep this path scoped to the qualified chip and firmware cap.
 */
struct brcmf_wcc_assoc_cmd {
	__le16 version;
	__le16 length;
	__le16 command;
	__le16 params;
};

struct brcmf_wcc_auth_start {
	struct brcmf_ssid_le ssid;
	u8 bssid[ETH_ALEN];
	u8 pad[2];
	__le32 akm;
	__le32 status;
};

struct brcmf_wcc_auth_tx {
	__le16 version;
	__le16 length;
	__le16 command;
	u8 frame[];
};

static bool brcmf_wcc_extsae(struct brcmf_pub *drvr)
{
	return drvr->bus_if->chip == BRCM_CC_43752_CHIP_ID &&
	       (drvr->feat_flags & BIT(BRCMF_FEAT_SAE_EXT));
}

static void brcmf_wcc_feat_attach(struct brcmf_if *ifp)
{
	char caps[768] = {};
	int err;

	if (ifp->drvr->bus_if->chip != BRCM_CC_43752_CHIP_ID)
		return;
	/* This firmware accepts P2P setup but never reports IF_ADD on VIM4.
	 * Avoid the failed P2P-device setup; STA and AP do not require P2P.
	 * Limit the workaround to this firmware so future images can requalify it.
	 */
	if (!strcmp(ifp->drvr->fwver, "01-16e134aa"))
		ifp->drvr->feat_flags &= ~BIT(BRCMF_FEAT_P2P);
	err = brcmf_fil_iovar_data_get(ifp, "cap", caps, sizeof(caps) - 1);
	if (!err && strstr(caps, "extsae ") &&
	    !(ifp->drvr->settings->feature_disable & BIT(BRCMF_FEAT_SAE_EXT)))
		ifp->drvr->feat_flags |= BIT(BRCMF_FEAT_SAE_EXT);
}

static int brcmf_wcc_assoc_control(struct brcmf_if *ifp, u16 command, u16 params)
{
	struct brcmf_wcc_assoc_cmd cmd = {
		.length = cpu_to_le16(sizeof(cmd)),
		.command = cpu_to_le16(command),
		.params = cpu_to_le16(params),
	};

	return brcmf_fil_iovar_data_set(ifp, "assoc_mgr_cmd", &cmd, sizeof(cmd));
}

static int brcmf_wcc_external_auth(struct wiphy *wiphy, struct net_device *ndev,
				   struct cfg80211_external_auth_params *params)
{
	struct brcmf_if *ifp = netdev_priv(ndev);

	if (!brcmf_wcc_extsae(ifp->drvr) || !is_valid_ether_addr(params->bssid) ||
	    !test_bit(BRCMF_VIF_STATUS_CONNECTING, &ifp->vif->sme_state))
		return -EINVAL;
	/* Resume association only after successful userspace authentication. */
	return brcmf_wcc_assoc_control(ifp, params->status == WLAN_STATUS_SUCCESS ?
				       BRCMF_WCC_ASSOC_PAUSE : BRCMF_WCC_ASSOC_ABORT, 0);
}

static int brcmf_wcc_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
			     struct cfg80211_mgmt_tx_params *params, u64 cookie)
{
	struct brcmf_if *ifp;
	const struct ieee80211_mgmt *mgmt = (const void *)params->buf;
	struct brcmf_wcc_auth_tx *cmd;
	size_t command_len;
	int err;

	if (params->len < sizeof(mgmt->frame_control))
		return -EINVAL;
	if (!ieee80211_is_auth(mgmt->frame_control))
		return brcmf_cfg80211_mgmt_tx(wiphy, wdev, params, cookie);
	if (!wdev->netdev)
		return -EINVAL;
	ifp = netdev_priv(wdev->netdev);
	if (!brcmf_wcc_extsae(ifp->drvr) || wdev->iftype != NL80211_IFTYPE_STATION ||
	    params->len < offsetof(struct ieee80211_mgmt, u.auth.variable) ||
	    params->len > IEEE80211_MAX_DATA_LEN ||
	    mgmt->u.auth.auth_alg != cpu_to_le16(WLAN_AUTH_SAE))
		return -EINVAL;

	/* Firmware takes a complete 802.11 authentication frame at params offset,
	 * with two trailing padding bytes included in the command buffer length.
	 */
	command_len = sizeof(*cmd) + params->len + sizeof(__le16);
	cmd = kzalloc(command_len, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	cmd->length = cpu_to_le16(params->len);
	cmd->command = cpu_to_le16(BRCMF_WCC_ASSOC_SEND_AUTH);
	memcpy(cmd->frame, params->buf, params->len);
	err = brcmf_fil_iovar_data_set(ifp, "assoc_mgr_cmd", cmd,
				       command_len);
	/* This firmware API acknowledges submission, not a radio TX ACK. */
	cfg80211_mgmt_tx_status(wdev, cookie, params->buf, params->len, !err, GFP_KERNEL);
	kfree(cmd);
	return err;
}

static s32 brcmf_wcc_auth_start(struct brcmf_if *ifp,
				const struct brcmf_event_msg *event, void *data)
{
	const struct brcmf_wcc_auth_start *start = data;
	struct cfg80211_external_auth_params params = {};
	int err;

	if (ifp->vif->profile.sec.auth_type != NL80211_AUTHTYPE_SAE)
		return 0;
	if (event->datalen < sizeof(*start) ||
	    le32_to_cpu(start->ssid.SSID_len) > IEEE80211_MAX_SSID_LEN ||
	    !is_valid_ether_addr(start->bssid))
		return -EINVAL;
	if (!test_bit(BRCMF_VIF_STATUS_CONNECTING, &ifp->vif->sme_state))
		return 0;
	params.action = NL80211_EXTERNAL_AUTH_START;
	params.key_mgmt_suite = WLAN_AKM_SUITE_SAE;
	params.ssid.ssid_len = le32_to_cpu(start->ssid.SSID_len);
	memcpy(params.ssid.ssid, start->ssid.SSID, params.ssid.ssid_len);
	ether_addr_copy(params.bssid, start->bssid);
	/* Pause before notifying userspace, so a fast reply cannot race the pause. */
	err = brcmf_wcc_assoc_control(ifp, BRCMF_WCC_ASSOC_PAUSE, 1);
	if (err)
		return err;
	err = cfg80211_external_auth_request(ifp->ndev, &params, GFP_KERNEL);
	if (err)
		brcmf_wcc_assoc_control(ifp, BRCMF_WCC_ASSOC_ABORT, 0);
	return err;
}

static s32 brcmf_wcc_auth_rx(struct brcmf_if *ifp,
			     const struct brcmf_event_msg *event, void *data)
{
	struct brcmf_cfg80211_info *cfg = ifp->drvr->config;
	struct ieee80211_mgmt *frame;
	struct brcmu_chan channel = {};
	enum nl80211_band band;
	u32 chanspec;
	int frequency, err;
	size_t len;

	if (event->auth_type != WLAN_AUTH_SAE || !event->datalen)
		return 0;
	if (event->datalen < 6 || event->datalen > IEEE80211_MAX_DATA_LEN)
		return -EINVAL;
	if (!test_bit(BRCMF_VIF_STATUS_CONNECTING, &ifp->vif->sme_state))
		return 0;
	err = brcmf_fil_iovar_int_get(ifp, "chanspec", &chanspec);
	if (err)
		return err;
	channel.chspec = chanspec;
	cfg->d11inf.decchspec(&channel);
	band = channel.band == BRCMU_CHAN_BAND_2G ? NL80211_BAND_2GHZ : NL80211_BAND_5GHZ;
	frequency = ieee80211_channel_to_frequency(channel.control_ch_num, band);
	if (!frequency)
		return -EINVAL;
	len = offsetof(struct ieee80211_mgmt, u) + event->datalen;
	frame = kzalloc(len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;
	frame->frame_control = cpu_to_le16(IEEE80211_STYPE_AUTH);
	ether_addr_copy(frame->da, ifp->mac_addr);
	ether_addr_copy(frame->sa, event->addr);
	ether_addr_copy(frame->bssid, event->addr);
	memcpy(&frame->u, data, event->datalen);
	cfg80211_rx_mgmt(&ifp->vif->wdev, frequency, 0, (u8 *)frame, len, 0);
	kfree(frame);
	return 0;
}

static void brcmf_wcc_get_cfg80211_ops(struct brcmf_pub *drvr)
{
	/* Called before firmware capability discovery. The callbacks themselves
	 * check extsae before accepting authentication operations.
	 */
	if (drvr->bus_if->chip != BRCM_CC_43752_CHIP_ID)
		return;
	drvr->ops->external_auth = brcmf_wcc_external_auth;
	drvr->ops->mgmt_tx = brcmf_wcc_mgmt_tx;
}

/* SAE is the final entry so the other interface types can retain the shared
 * WPA AKMs without advertising the station-only external authentication path.
 */
static const u32 brcmf_wcc_akms[] = {
	WLAN_AKM_SUITE_8021X,
	WLAN_AKM_SUITE_PSK,
	WLAN_AKM_SUITE_FT_8021X,
	WLAN_AKM_SUITE_FT_PSK,
	WLAN_AKM_SUITE_8021X_SHA256,
	WLAN_AKM_SUITE_PSK_SHA256,
	WLAN_AKM_SUITE_WFA_DPP,
	WLAN_AKM_SUITE_SAE,
};

static const struct wiphy_iftype_akm_suites brcmf_wcc_iftype_akms[] = {
	{
		.iftypes_mask = BIT(NL80211_IFTYPE_STATION),
		.akm_suites = brcmf_wcc_akms,
		.n_akm_suites = ARRAY_SIZE(brcmf_wcc_akms),
	}, {
		.iftypes_mask = BIT(NL80211_IFTYPE_AP) | BIT(NL80211_IFTYPE_ADHOC) |
				BIT(NL80211_IFTYPE_P2P_GO) | BIT(NL80211_IFTYPE_P2P_CLIENT),
		.akm_suites = brcmf_wcc_akms,
		.n_akm_suites = ARRAY_SIZE(brcmf_wcc_akms) - 1,
	},
};

static void brcmf_wcc_setup_wiphy(struct brcmf_pub *drvr, struct wiphy *wiphy)
{
	if (!brcmf_wcc_extsae(drvr))
		return;
	/* Older supplicants do not infer SAE AKM support from the SAE feature
	 * flag for a fullmac device using the CONNECT command.
	 */
	wiphy->iftype_akm_suites = brcmf_wcc_iftype_akms;
	wiphy->num_iftype_akm_suites = ARRAY_SIZE(brcmf_wcc_iftype_akms);
}

static void brcmf_wcc_register_events(struct brcmf_pub *drvr)
{
	if (!brcmf_wcc_extsae(drvr))
		return;
	brcmf_fweh_register(drvr, BRCMF_E_JOIN_START, brcmf_wcc_auth_start);
	brcmf_fweh_register(drvr, BRCMF_E_AUTH, brcmf_wcc_auth_rx);
}

static int brcmf_wcc_set_sae_pwd(struct brcmf_if *ifp,
				 struct cfg80211_crypto_settings *crypto)
{
	return brcmf_set_wsec(ifp, crypto->sae_pwd, crypto->sae_pwd_len,
			      BRCMF_WSEC_PASSPHRASE);
}

static int brcmf_wcc_alloc_fweh_info(struct brcmf_pub *drvr)
{
	struct brcmf_fweh_info *fweh;

	fweh = kzalloc_flex(*fweh, evt_handler, BRCMF_WCC_E_LAST);
	if (!fweh)
		return -ENOMEM;

	fweh->num_event_codes = BRCMF_WCC_E_LAST;
	drvr->fweh = fweh;
	return 0;
}

const struct brcmf_fwvid_ops brcmf_wcc_ops = {
	.feat_attach = brcmf_wcc_feat_attach,
	.set_sae_password = brcmf_wcc_set_sae_pwd,
	.alloc_fweh_info = brcmf_wcc_alloc_fweh_info,
	.get_cfg80211_ops = brcmf_wcc_get_cfg80211_ops,
	.setup_wiphy = brcmf_wcc_setup_wiphy,
	.register_event_handlers = brcmf_wcc_register_events,
};
