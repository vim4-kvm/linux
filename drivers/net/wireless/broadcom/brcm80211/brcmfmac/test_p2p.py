#!/usr/bin/env python3
"""Compile actual P2P request builders against a recording firmware boundary."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).parent


def block(file, signature, end='\n}'):
    source = (root / file).read_text()
    start = source.index(signature)
    return source[start:source.index(end, start) + len(end)]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define DECLARE_FLEX_ARRAY(t, name) struct { struct {} __empty_ ## name; t name[]; }
#define cpu_to_le16(v) ((uint16_t)(v))
#define cpu_to_le32(v) ((uint32_t)(v))
#define le32_to_cpu(v) ((uint32_t)(v))
#define roundup(v, n) (((v) + (n) - 1) / (n) * (n))
#define GFP_KERNEL 0
#define kfree free
#define brcmf_dbg(...) ((void)0)
#define bphy_err(...) ((void)0)
#define BRCMF_SCAN_PARAMS_COUNT_MASK 0xffff
#define BRCMF_SCAN_PARAMS_NSSID_SHIFT 16
#define BRCMF_FEAT_SCAN_V2 0
#define BRCMF_FEAT_QUIRK_ARP_ND_PRIMARY 0
#define BRCMF_FEAT_QUIRK_P2P_DEV_NO_IF_EVT 1
#define BRCMF_FEAT_SAE_EXT 2
#define BRCM_CC_43752_CHIP_ID 43752
#define BIT(n) (1UL << (n))
#define BRCMF_VIF_STATUS_CONNECTED 0
#define BRCMF_SCAN_STATUS_BUSY 0
#define BRCMF_P2P_STATUS_ENABLED 0
#define BRCMF_C_DOWN 2
#define BRCMF_C_UP 3
#define BRCMF_ARP_OL_AGENT 1
#define BRCMF_ARP_OL_PEER_AUTO_REPLY 2
static int fail_alloc = -1, allocations;
static void *kzalloc(size_t n, int flags) {
    (void)flags;
    return allocations++ == fail_alloc ? NULL : calloc(1, n);
}
struct brcmf_if;
struct brcmf_bus { unsigned int chip; };
struct brcmf_mp_device { int fcmode; unsigned long feature_disable; };
struct brcmf_pub { void *wiphy; struct brcmf_if *iflist[16];
    struct brcmf_bus *bus_if; struct brcmf_mp_device *settings;
    const char *fwver; unsigned long chip_quirks, feat_flags; };
struct brcmf_if { struct brcmf_pub *drvr; int ifidx; bool v2, primary_nd; };
struct brcmf_cfg80211_vif { struct brcmf_if *ifp; };
struct brcmf_cfg80211_info { struct brcmf_pub *pub; unsigned long scan_status; };
enum p2p_bss_type { P2PAPI_BSSCFG_PRIMARY, P2PAPI_BSSCFG_DEVICE, P2PAPI_BSSCFG_MAX };
struct brcmf_p2p_info { struct brcmf_cfg80211_info *cfg; unsigned long status;
    struct { struct brcmf_cfg80211_vif *vif; } bss_idx[P2PAPI_BSSCFG_MAX]; };
static bool connected, ap_running;
static unsigned int apsta;
static int op_count, fail_op = -1;
static struct { const char *name; int ifidx; unsigned int value; } ops[32];
static int record(struct brcmf_if *ifp, const char *name, u32 value) {
    int i = op_count++;
    assert(i < 32);
    ops[i].name = name; ops[i].ifidx = ifp->ifidx; ops[i].value = value;
    return i == fail_op ? -EIO : 0;
}
static int brcmf_fil_iovar_int_get(struct brcmf_if *ifp, const char *name, u32 *v) {
    *v = apsta; return record(ifp, name, 0);
}
static int brcmf_fil_cmd_int_set(struct brcmf_if *ifp, int cmd, int value) {
    return record(ifp, cmd == BRCMF_C_DOWN ? "DOWN" : "UP", value);
}
static int brcmf_fil_iovar_int_set(struct brcmf_if *ifp, const char *name, u32 value) {
    return record(ifp, name, value);
}
static int brcmf_fil_iovar_data_set(struct brcmf_if *ifp, const char *name,
                                   const void *data, size_t n) {
    (void)data; return record(ifp, name, n);
}
static int caps_error;
static const char *firmware_caps = "p2p proptxstatus extsae ";
static int brcmf_fil_iovar_data_get(struct brcmf_if *ifp, const char *name,
                                   void *data, size_t n) {
    (void)ifp; assert(!strcmp(name, "cap"));
    assert(strlen(firmware_caps) < n);
    if (!caps_error) memcpy(data, firmware_caps, strlen(firmware_caps));
    return caps_error;
}
static unsigned char request[1024];
static size_t request_len;
static int scan_error, scan_calls;
static int brcmf_fil_bsscfg_data_set(struct brcmf_if *ifp, const char *name,
                                    const void *data, size_t n) {
    (void)ifp; assert(!strcmp(name, "p2p_scan")); assert(n <= sizeof(request));
    memcpy(request, data, n); request_len = n; scan_calls++; return scan_error;
}
static bool brcmf_feat_is_enabled(struct brcmf_if *ifp, int feature) {
    (void)feature; return ifp->v2;
}
static bool brcmf_feat_is_quirk_enabled(struct brcmf_if *ifp, int quirk) {
    (void)quirk; return ifp->primary_nd;
}
static bool brcmf_is_apmode_operating(void *wiphy) { (void)wiphy; return ap_running; }
static bool brcmf_get_vif_state_any(struct brcmf_cfg80211_info *cfg, int state) {
    (void)cfg; (void)state; return connected;
}
static int discover_error;
static int brcmf_p2p_set_discover_state(struct brcmf_if *ifp, int state, int c, int t) {
    (void)ifp; (void)state; (void)c; (void)t; return discover_error;
}
static void eth_broadcast_addr(u8 *a) { memset(a, 0xff, ETH_ALEN); }
static void set_bit(int bit, unsigned long *value) { *value |= 1UL << bit; }
static void clear_bit(int bit, unsigned long *value) { *value &= ~(1UL << bit); }
'''
# Keep the wire layouts and constants identical to the driver's source.
wire = (root / 'fwil_types.h').read_text()
constants = []
for line in wire.splitlines():
    if line.startswith('#define ') and line.split()[1] in {
        'BRCMF_SCAN_PARAMS_FIXED_SIZE', 'BRCMF_SCAN_PARAMS_V2_FIXED_SIZE',
        'BRCMF_SCAN_PARAMS_VERSION_V2', 'BRCMF_SCANTYPE_ACTIVE',
        'BRCMF_ESCAN_REQ_VERSION', 'BRCMF_ESCAN_REQ_VERSION_V2'}:
        constants.append(line)
p2p = (root / 'p2p.c').read_text()
for line in p2p.splitlines():
    if line.startswith('#define ') and line.split()[1] in {
        'P2PAPI_SCAN_HOME_TIME_MS', 'P2PAPI_SCAN_SOCIAL_DWELL_TIME_MS',
        'P2PAPI_SCAN_AF_SEARCH_DWELL_TIME_MS', 'P2PAPI_SCAN_DWELL_TIME_MS',
        'WL_SCAN_CONNECT_DWELL_TIME_MS', 'WL_SCAN_JOIN_PROBE_INTERVAL_MS',
        'P2PAPI_SCAN_NPROBS_TIME_MS', 'BRCMF_P2P_WILDCARD_SSID',
        'BRCMF_P2P_WILDCARD_SSID_LEN', 'SOCIAL_CHAN_CNT', 'AF_PEER_SEARCH_CNT'}:
        constants.append(line)
constants += ['#define DOT11_BSSTYPE_ANY 2', '#define WL_ESCAN_ACTION_START 1']
layouts = '\n'.join([
    block('fwsignal.h', 'enum brcmf_fws_fcmode {', '\n};'),
    block('fwil_types.h', 'struct brcmf_ssid_le {', '\n};'),
    block('fwil_types.h', 'struct brcmf_scan_params_le {', '\n};'),
    block('fwil_types.h', 'struct brcmf_scan_params_v2_le {', '\n};'),
    block('fwil_types.h', 'struct brcmf_escan_params_le {', '\n};'),
    block('p2p.c', 'struct brcmf_p2p_scan_le {', '\n};'),
    block('p2p.c', 'enum brcmf_p2p_disc_state {', '\n};'),
])
functions = '\n'.join([
    block('wcc/core.c', 'static void brcmf_wcc_feat_attach('),
    block('cfg80211.c', 'void brcmf_scan_params_v2_to_v1('),
    block('p2p.c', 'static int brcmf_p2p_set_firmware('),
    block('p2p.c', 'static s32 brcmf_p2p_escan('),
    block('p2p.c', 'static s32 brcmf_p2p_deinit_discovery('),
    block('core.c', 'void brcmf_configure_arp_nd_offload('),
])
tests = r'''
int main(void) {
    struct brcmf_pub pub = {0};
    struct brcmf_if primary = {.drvr=&pub, .ifidx=0}, group = {.drvr=&pub, .ifidx=1};
    struct brcmf_cfg80211_info cfg = {.pub=&pub};
    struct brcmf_cfg80211_vif vif = {.ifp=&primary};
    struct brcmf_p2p_info p2p = {.cfg=&cfg};
    u8 mac[ETH_ALEN] = {2, 1, 2, 3, 4, 5};
    pub.iflist[0] = &primary;
    p2p.bss_idx[P2PAPI_BSSCFG_DEVICE].vif = &vif;
    p2p.bss_idx[P2PAPI_BSSCFG_PRIMARY].vif = &vif;
    /* Recreated discovery must initialize wsec and its firmware state again. */
    p2p.status=1;
    assert(brcmf_p2p_deinit_discovery(&p2p) == 0 && !p2p.status);
    assert(op_count == 1 && !strcmp(ops[0].name,"p2p_disc"));
    op_count=0;
    /* An existing APSTA configuration must not take the linked STA down. */
    apsta = 1;
    assert(brcmf_p2p_set_firmware(&primary, mac) == 0);
    assert(op_count == 3);
    assert(!strcmp(ops[1].name, "p2p_disc"));
    for (int i=0; i<op_count; i++) assert(strcmp(ops[i].name, "DOWN"));
    /* Failures propagate; UP is attempted even when setting APSTA failed. */
    apsta = 0;
    for (int failure=0; failure<6; failure++) {
        op_count=0; fail_op=failure;
        assert(brcmf_p2p_set_firmware(&primary, mac) == -EIO);
        if (failure == 2) assert(!strcmp(ops[3].name, "UP"));
    }
    fail_op=-1;
    /* The discovered scan ABI governs P2P too, for odd/even channel counts. */
    for (int v2=0; v2<2; v2++) for (int n=1; n<=64; n++) {
        u16 channels[64];
        for (int i=0; i<n; i++) channels[i] = 0x1000 + i;
        primary.v2=v2; allocations=0; fail_alloc=-1; scan_calls=0;
        cfg.scan_status=0;
        assert(brcmf_p2p_escan(&p2p,n,channels,WL_P2P_DISC_ST_SCAN,
                               P2PAPI_BSSCFG_DEVICE) == 0);
        assert(scan_calls == 1 && cfg.scan_status == 1);
        /* Independent wire offsets catch an ABI version/layout mismatch. */
        assert(request[0] == 'E');
        assert(*(u32 *)(request+4) == (u32)(v2 ? 2 : 1));
        assert(*(u16 *)(request+8) == 1);
        assert(*(u16 *)(request+10) == 0x1234);
        size_t base = 12;
        if (v2) {
            assert(*(u16 *)(request+base) == 2);
            assert(*(u16 *)(request+base+2) == 72);
            base += 4;
        }
        assert(*(u32 *)(request+base) == 7);
        assert(!memcmp(request+base+4,"DIRECT-",7));
        size_t fixed = v2 ? 72 : 64;
        assert(request_len == 12 + fixed + 2*n);
        assert(*(u32 *)(request+12+fixed-4) == (u32)n);
        assert(!memcmp(request+12+fixed,channels,2*n));
    }
    u16 channel=0x1001;
    for (int failure=0; failure<2; failure++) {
        primary.v2=false; allocations=0; fail_alloc=failure; scan_calls=0;
        cfg.scan_status=0;
        assert(brcmf_p2p_escan(&p2p,1,&channel,WL_P2P_DISC_ST_SCAN,
                               P2PAPI_BSSCFG_DEVICE) == -ENOMEM);
        assert(!scan_calls && !cfg.scan_status);
    }
    fail_alloc=-1; discover_error=-EIO; cfg.scan_status=0; scan_calls=0;
    assert(brcmf_p2p_escan(&p2p,1,&channel,WL_P2P_DISC_ST_SCAN,
                           P2PAPI_BSSCFG_DEVICE) == -EIO);
    assert(!scan_calls && !cfg.scan_status);
    discover_error=0; scan_error=-EBUSY; cfg.scan_status=0;
    assert(brcmf_p2p_escan(&p2p,1,&channel,WL_P2P_DISC_ST_SCAN,
                           P2PAPI_BSSCFG_DEVICE) == -EBUSY);
    assert(!cfg.scan_status);
    /* Radio-wide offload controls use primary, without losing group data. */
    group.primary_nd=true; op_count=0;
    brcmf_configure_arp_nd_offload(&group,false);
    assert(op_count == 3);
    assert(ops[0].ifidx == 0 && ops[1].ifidx == 0);
    assert(!strcmp(ops[2].name,"ndoe") && ops[2].ifidx == 0);
    group.primary_nd=false; op_count=0;
    brcmf_configure_arp_nd_offload(&group,false);
    assert(ops[2].ifidx == 1);
    ap_running=true; op_count=0;
    brcmf_configure_arp_nd_offload(&group,true);
    assert(!op_count);
    /* Automatic flow control is scoped to the qualified radio/firmware.
     * Explicit module parameter values remain authoritative.
     */
    struct brcmf_bus bus = {.chip=BRCM_CC_43752_CHIP_ID};
    struct brcmf_mp_device settings = {.fcmode=BRCMF_FWS_FCMODE_AUTO};
    pub.bus_if=&bus; pub.settings=&settings; pub.fwver="01-16e134aa";
    brcmf_wcc_feat_attach(&primary);
    assert(settings.fcmode == BRCMF_FWS_FCMODE_EXPLICIT_CREDIT);
    assert(pub.chip_quirks & BIT(BRCMF_FEAT_QUIRK_P2P_DEV_NO_IF_EVT));
    assert(pub.chip_quirks & BIT(BRCMF_FEAT_QUIRK_ARP_ND_PRIMARY));
    assert(pub.feat_flags & BIT(BRCMF_FEAT_SAE_EXT));
    for (int mode=0; mode<=2; mode++) {
        settings.fcmode=mode;
        brcmf_wcc_feat_attach(&primary);
        assert(settings.fcmode == mode);
    }
    for (int failure=0; failure<4; failure++) {
        settings.fcmode=BRCMF_FWS_FCMODE_AUTO;
        bus.chip=failure == 0 ? 1234 : BRCM_CC_43752_CHIP_ID;
        pub.fwver=failure == 1 ? "unknown" : "01-16e134aa";
        caps_error=failure == 2 ? -EIO : 0;
        firmware_caps=failure == 3 ? "p2p extsae " : "p2p proptxstatus extsae ";
        brcmf_wcc_feat_attach(&primary);
        assert(settings.fcmode == BRCMF_FWS_FCMODE_AUTO);
    }
    settings.feature_disable=BIT(BRCMF_FEAT_SAE_EXT); pub.feat_flags=0;
    brcmf_wcc_feat_attach(&primary);
    assert(!(pub.feat_flags & BIT(BRCMF_FEAT_SAE_EXT)));
    puts("P2P scan ABI, lifecycle, offload routing and TX credit policy passed");
}
'''
with tempfile.TemporaryDirectory(prefix='brcmf-p2p-') as tmp:
    program = Path(tmp) / 'p2p.c'
    binary = Path(tmp) / 'p2p'
    program.write_text('\n'.join([prefix, *constants, layouts, functions, tests]))
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-variable', '-fsanitize=address,undefined',
                    str(program), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
