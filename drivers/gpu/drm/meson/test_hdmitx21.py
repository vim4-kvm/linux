#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the TX transport and DDC state machine with deterministic firmware replies."""
from pathlib import Path
import argparse
import subprocess
import tempfile

source = Path(__file__).with_name('meson_hdmitx21.c').read_text()


def functions(start, end):
    return source[source.index(start):source.index(end)]


transport = functions('static u32 meson_hdmitx21_smc_read', 'static u32 phy_read')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint64_t resource_size_t;
struct meson_hdmitx21 { resource_size_t boot_top, boot_cor; };
struct arm_smccc_res { unsigned long a0; };
static unsigned long call[8];
static void arm_smccc_smc(unsigned long a, unsigned long b, unsigned long c,
                         unsigned long d, unsigned long e, unsigned long f,
                         unsigned long g, unsigned long h, struct arm_smccc_res *res)
{
    unsigned long args[] = {a,b,c,d,e,f,g,h};
    for (int i = 0; i < 8; i++) call[i] = args[i];
    res->a0 = 0x87654321;
}
'''
program += transport
program += r'''
int main(void)
{
    struct meson_hdmitx21 tx = {0xfe300000, 0xfe380000};
    assert(top_read(&tx, 0x8c) == 0x87654321);
    assert(call[0] == 0x82000018 && call[1] == 0xfe30008c && call[2] == 32);
    assert(cor_read(&tx, 0x61a) == 0x21);
    assert(call[1] == 0xfe38061a && call[2] == 8 && call[3] == 0);
    top_write(&tx, 0x14, 0x12345678);
    assert(call[0] == 0x82000019 && call[1] == 0xfe300014);
    assert(call[2] == 0x12345678 && call[3] == 32);
    cor_write(&tx, 0x9, 0x302);
    assert(call[1] == 0xfe380009 && call[2] == 2 && call[3] == 8);
    for (int i = 4; i < 8; i++) assert(call[i] == 0);
    puts("TX firmware transport: TOP32/COR8 read and write arguments passed");
}
'''
def run_test(name, program):
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / (name + '.c')
        c.write_text(program)
        binary = Path(tmp) / name
        subprocess.run(['cc', '-Wno-overflow', str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


run_test('transport', program)
hpd = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
enum drm_connector_status {
    connector_status_connected, connector_status_disconnected, connector_status_unknown
};
struct drm_connector { int unused; };
struct drm_bridge { int unused; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct gpio_desc { int level; };
struct meson_hdmitx21 {
    struct drm_bridge bridge;
    struct gpio_desc *hpd;
    struct delayed_work hpd_work;
    enum drm_connector_status hpd_sample, hpd_reported;
    bool hpd_polling;
};
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define to_delayed_work(ptr) container_of(ptr, struct delayed_work, work)
#define bridge_to_hdmitx(ptr) container_of(ptr, struct meson_hdmitx21, bridge)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define msecs_to_jiffies(x) (x)
static int queued, cancelled, notifications, disconnected;
static enum drm_connector_status notified;
static int gpiod_get_value_cansleep(struct gpio_desc *gpio) { return gpio->level; }
static void meson_hdmitx21_audio_disconnect(struct meson_hdmitx21 *tx) { disconnected++; }
static void schedule_delayed_work(struct delayed_work *w, int delay) { queued++; }
static void cancel_delayed_work_sync(struct delayed_work *w) { cancelled++; }
static void drm_bridge_hpd_notify(struct drm_bridge *b, enum drm_connector_status s)
{ notifications++; notified = s; }
'''
hpd += functions('static enum drm_connector_status\nmeson_hdmitx21_bridge_detect',
                 'static const struct drm_edid *')
hpd += functions('static void meson_hdmitx21_hpd_work',
                 'static void meson_hdmitx21_monitor_cleanup')
hpd += r'''
int main(void)
{
    struct gpio_desc gpio = {0};
    struct meson_hdmitx21 tx = {.hpd = &gpio};
    meson_hdmitx21_monitor_hpd_enable(&tx.bridge);
    assert(queued == 1 && tx.hpd_polling);
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    assert(!notifications);
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    assert(notifications == 1 && notified == connector_status_disconnected);
    gpio.level = 1; meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    gpio.level = 0; meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    gpio.level = 1; meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    assert(notifications == 1); /* bouncing does not produce a plug event */
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    assert(notifications == 2 && notified == connector_status_connected);
    gpio.level = -5;
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    assert(notifications == 2); /* GPIO read failures are not unplug events */
    gpio.level = 0;
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    meson_hdmitx21_hpd_work(&tx.hpd_work.work);
    assert(notifications == 3 && notified == connector_status_disconnected);
    assert(disconnected == 2);
    meson_hdmitx21_monitor_hpd_disable(&tx.bridge);
    assert(cancelled == 1 && !tx.hpd_polling);
    puts("HPD: cold unplug, bounce, plug, read error, unplug and synchronous stop passed");
}
'''
run_test('hpd', hpd)

ddc = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define I2C_M_RD 1
#define I2C_M_DMA_SAFE 0x200
struct meson_hdmitx21 { int ddc_lock; };
struct i2c_adapter { struct meson_hdmitx21 *tx; };
struct i2c_msg { unsigned short addr, flags, len; u8 *buf; };
#define i2c_get_adapdata(adap) ((adap)->tx)
static int locks, commands, sleeps, remaining, position, failure;
static u8 registers[0x1000];
static void mutex_lock(int *p) { assert(!locks); locks++; }
static void mutex_unlock(int *p) { assert(locks == 1); locks--; }
static void usleep_range(int lo, int hi) { assert(++sleeps < 300); }
'''
ddc += '\n'.join(line for line in source.splitlines() if line.startswith('#define ')) + '\n'
ddc += r'''
static u8 cor_read(struct meson_hdmitx21 *tx, u32 reg)
{
    if (reg == DDC_STATUS) return failure == 1 ? DDC_STATUS_NACK :
                                 failure == 2 ? DDC_STATUS_BUSY : 0;
    if (reg == DDC_DOUT_CNT) return failure == 1 || failure == 3 ? 0 :
                                   failure == 4 ? 17 : remaining > 16 ? 16 : remaining;
    if (reg == DDC_DATA) {
        assert(remaining > 0); remaining--;
        return registers[DDC_SEGM] ^ position++;
    }
    return registers[reg];
}
static void cor_write(struct meson_hdmitx21 *tx, u32 reg, u8 val)
{
    registers[reg] = val;
    if (reg != DDC_CMD) return;
    commands++;
    if (val == DDC_CMD_SEQ_READ || val == DDC_CMD_ENH_READ) {
        remaining = registers[DDC_DIN_CNT1] + 256 * registers[DDC_DIN_CNT2];
        position = registers[DDC_OFFSET];
    }
}
static void cor_update(struct meson_hdmitx21 *tx, u32 reg, u8 mask, u8 val)
{ cor_write(tx, reg, (cor_read(tx, reg) & ~mask) | (val & mask)); }
'''
ddc += functions('static void meson_hdmitx21_ddc_cmd', '/* One-byte DDC write')
ddc += functions('static int meson_hdmitx21_i2c_xfer', 'static u32 meson_hdmitx21_i2c_func')
ddc += r'''
int main(void)
{
    struct meson_hdmitx21 tx = {0};
    struct i2c_adapter adapter = {&tx};
    u8 segment = 0, offset = 0, data[128];
    struct i2c_msg msgs[] = {{0x30,0,1,&segment}, {0x50,0,1,&offset},
                             {0x50,I2C_M_RD,128,data}};
    for (int block = 0; block < 8; block++) {
        segment = block / 2; offset = (block % 2) * 128;
        sleeps = 0;
        assert(meson_hdmitx21_i2c_xfer(&adapter, msgs + !segment, 3 - !segment)
               == 3 - !segment);
        for (int i = 0; i < 128; i++) assert(data[i] == (u8)(segment ^ (offset + i)));
        assert(!locks && !(registers[TPI_DDC_MASTER_EN] & TPI_DDC_HW_EN));
    }
    offset = 0; segment = 0;
    for (failure = 1; failure <= 4; failure++) {
        int expected[] = {0, -ENXIO, -EBUSY, -ETIMEDOUT, -EIO};
        sleeps = 0;
        assert(meson_hdmitx21_i2c_xfer(&adapter, msgs + 1, 2) == expected[failure]);
        assert(!locks && !(registers[TPI_DDC_MASTER_EN] & TPI_DDC_HW_EN));
    }
    failure = 0;
    int before = commands;
    assert(meson_hdmitx21_i2c_xfer(&adapter, msgs, 1) == -EOPNOTSUPP);
    msgs[2].flags |= 0x10;
    assert(meson_hdmitx21_i2c_xfer(&adapter, msgs, 3) == -EOPNOTSUPP);
    msgs[2].flags = I2C_M_RD; offset = 200;
    assert(meson_hdmitx21_i2c_xfer(&adapter, msgs, 3) == -EOPNOTSUPP);
    offset = 0; msgs[2].len = 0;
    assert(meson_hdmitx21_i2c_xfer(&adapter, msgs, 3) == -EOPNOTSUPP);
    assert(commands == before);
    puts("DDC: 8 EDID blocks, NACK, busy, empty FIFO timeout, FIFO overrun and malformed transfers passed");
}
'''
run_test('ddc', ddc)

audio = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#define HDMI_I2S 1
#define DRM_BRIDGE_OP_HDMI_AUDIO 1
struct drm_bridge { unsigned int ops; };
struct drm_connector { int unused; };
struct hdmi_codec_daifmt { int fmt; };
struct hdmi_codec_params { int channels, sample_rate, sample_width; };
struct meson_hdmitx21 {
    struct drm_bridge bridge;
    int audio_lock, pixel_khz;
    bool sink_hdmi, audio_enabled, audio_muted;
    struct hdmi_codec_params audio_params;
};
#define bridge_to_hdmitx(p) ((struct meson_hdmitx21 *)((char *)(p) - offsetof(struct meson_hdmitx21, bridge)))
static int locked, configured, muted, stopped;
static void mutex_lock(int *lock) { assert(!locked); locked++; }
static void mutex_unlock(int *lock) { assert(locked == 1); locked--; }
static void meson_hdmitx21_audio_config(struct meson_hdmitx21 *tx) { configured++; }
static void meson_hdmitx21_audio_off(struct meson_hdmitx21 *tx) { stopped++; }
static void meson_hdmitx21_audio_mute(struct meson_hdmitx21 *tx, bool mute) { muted++; }
'''
audio += functions('static int meson_hdmitx21_audio_prepare', '/* Bridge */')
audio += functions('static void meson_hdmitx21_audio_disconnect',
                   'static int meson_hdmitx21_audio_prepare')
audio += r'''
int main(void)
{
    struct meson_hdmitx21 tx = {0};
    struct hdmi_codec_daifmt fmt = {HDMI_I2S};
    struct hdmi_codec_params params = {2,48000,24};
    /* Monitor-only handoff must not acquire audio register ownership. */
    meson_hdmitx21_audio_mode(&tx, 148500, true);
    meson_hdmitx21_audio_disconnect(&tx);
    assert(!tx.pixel_khz && !configured && !stopped);
    tx.bridge.ops = DRM_BRIDGE_OP_HDMI_AUDIO;
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -ENODEV);
    tx.pixel_khz = 148500; /* DVI must not transmit audio packets */
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -ENODEV);
    tx.sink_hdmi = true;
    int rates[] = {32000,44100,48000,88200,96000,176400,192000};
    for (int i = 0; i < 7; i++) {
        params.sample_rate = rates[i];
        assert(!meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params));
    }
    assert(configured == 7 && tx.audio_enabled && !locked);
    /* HPD loss immediately closes prepare, even before userspace disables KMS. */
    meson_hdmitx21_audio_disconnect(&tx);
    assert(!tx.pixel_khz && !tx.sink_hdmi && stopped == 1);
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -ENODEV);
    meson_hdmitx21_audio_mode(&tx, 297000, true);
    assert(tx.pixel_khz == 297000 && tx.sink_hdmi && configured == 8);
    meson_hdmitx21_audio_mode(&tx, 148500, false);
    assert(!tx.sink_hdmi && stopped == 2);
    meson_hdmitx21_audio_mode(&tx, 148500, true);
    params.sample_rate = 12345;
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -EINVAL);
    params.sample_rate = 48000; params.sample_width = 32;
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -EINVAL);
    params.sample_width = 16; params.channels = 8;
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -EINVAL);
    params.channels = 2; fmt.fmt = 0;
    assert(meson_hdmitx21_audio_prepare(&tx.bridge, NULL, &fmt, &params) == -EINVAL);
    assert(configured == 9);
    meson_hdmitx21_audio_mute_stream(&tx.bridge, NULL, true, 0);
    assert(tx.audio_muted && muted == 1);
    tx.pixel_khz = 0;
    meson_hdmitx21_audio_mute_stream(&tx.bridge, NULL, false, 0);
    assert(!tx.audio_muted && muted == 1);
    meson_hdmitx21_audio_shutdown(&tx.bridge, NULL);
    assert(!tx.audio_enabled && stopped == 3 && !locked);
    puts("HDMI codec: inactive/DVI refusal, PCM rate/width/channel bounds and mute/shutdown passed");
}
'''
run_test('audio', audio)

# Exercise the real audio register sequence, including the shared packet window.
audio_hw = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define HDMI_INFOFRAME_SIZE(type) 14
#define dev_dbg(...) ((void)0)
struct hdmi_codec_params {
    unsigned int sample_rate, sample_width;
    struct { u8 status[5]; } iec;
    struct { int unused; } cea;
};
struct meson_hdmitx21 {
    struct hdmi_codec_params audio_params;
    unsigned int pixel_khz;
    bool sink_hdmi, audio_muted;
    int packet_lock;
};
static u8 cor[4096], packets[3][31];
static u32 top;
static unsigned int writes, locked, acr_calls;
static void mutex_lock(int *lock) { assert(!locked); locked++; }
static void mutex_unlock(int *lock) { assert(locked == 1); locked--; }
static u32 top_read(struct meson_hdmitx21 *tx, u32 reg) { return top; }
static void top_write(struct meson_hdmitx21 *tx, u32 reg, u32 value) { top=value; writes++; }
static void cor_write(struct meson_hdmitx21 *tx, u32 reg, u8 value) {
    if (reg >= 0x6c0 && reg <= 0x6df) {
        assert(locked && cor[0x6bf] < 3);
        if (reg < 0x6df) packets[cor[0x6bf]][reg - 0x6c0] = value;
    }
    cor[reg] = value; writes++;
}
static void cor_update(struct meson_hdmitx21 *tx, u32 reg, u8 mask, u8 value)
{ cor_write(tx, reg, (cor[reg] & ~mask) | (value & mask)); }
static void drm_hdmi_acr_get_n_cts(unsigned long long clock, unsigned int rate,
                                  unsigned int *n, unsigned int *cts) {
    assert(clock == 148500000 || clock == 297000000 || clock == 594000000);
    assert(rate == 48000); *n=6144; *cts=clock / 1000; acr_calls++;
}
static ssize_t hdmi_audio_infoframe_pack(void *frame, u8 *buf, size_t size) {
    for (unsigned int i=0; i<size; i++) buf[i]=i + 0x80;
    return size;
}
'''
audio_hw += '\n'.join(line for line in source.splitlines() if line.startswith('#define '))
audio_hw += '\n' + functions('static void meson_hdmitx21_write_infoframe',
                             'static void meson_hdmitx21_write_avi')
audio_hw += functions('static void meson_hdmitx21_audio_mute(',
                      'static void meson_hdmitx21_audio_disconnect')
audio_hw += r'''
int main(void) {
    struct meson_hdmitx21 tx = {.audio_params = {.sample_rate=48000, .sample_width=24}};
    meson_hdmitx21_audio_config(&tx); assert(!writes);
    tx.pixel_khz=148500;
    meson_hdmitx21_audio_config(&tx); assert(!writes); /* DVI */
    tx.sink_hdmi=true; tx.audio_muted=true;
    unsigned int clocks[] = {148500,297000,594000};
    for (unsigned int i=0; i<3; i++) {
        tx.pixel_khz=clocks[i]; top=~0U;
        meson_hdmitx21_audio_config(&tx);
        assert(cor[0xa03] == 0 && cor[0xa04] == 24 && cor[0xa05] == 0);
        assert(cor[0xa01] == 2 && !cor[0xa13] && cor[0xa2f] == 0x80);
        assert(!(cor[0xa2c] & 1) && !(top & (1 << 13)));
        assert(packets[2][0] == 0x80 && packets[2][13] == 0x8d && !packets[2][14]);
        assert(!locked && !packets[0][0]);
    }
    tx.audio_muted=false;
    meson_hdmitx21_audio_config(&tx);
    assert(cor[0xa13] == 3 && !(cor[0xa2f] & 0x80) && !(cor[0xa62] & 0x10));
    meson_hdmitx21_audio_off(&tx);
    assert(!cor[0xa13] && !(cor[0xa01] & 2) && !cor[0x6df] && !locked);
    assert(acr_calls == 4);
    puts("Audio registers: ACR clocks, DVI gating, persistent mute and packet-window locking passed");
}
'''
run_test('audio_hw', audio_hw)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dtb', type=Path, help='M2 compiled DTB with TX audio')
parser.add_argument('--baseline-dtb', type=Path, help='M1 compiled DTB for RX comparison')
args = parser.parse_args()
if bool(args.dtb) != bool(args.baseline_dtb):
    parser.error('--dtb and --baseline-dtb must be supplied together')
if args.dtb:
    def prop(dtb, path, name, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), path, name],
                                       text=True).strip()

    def nodes(dtb, path='/'):
        yield path
        children = subprocess.check_output(['fdtget', '-l', str(dtb), path], text=True).split()
        for child in children:
            yield from nodes(dtb, path.rstrip('/') + '/' + child)

    def phandles(dtb):
        result = {}
        for path in nodes(dtb):
            names = subprocess.check_output(['fdtget', '-p', str(dtb), path], text=True).split()
            if 'phandle' in names:
                result[int(prop(dtb, path, 'phandle', 'x'), 16)] = path
        return result

    def clock_spec(dtb, path, name):
        cells = [int(cell, 16) for cell in prop(dtb, path, name, 'x').split()]
        handles = phandles(dtb)
        result = []
        while cells:
            provider = handles[cells.pop(0)]
            count = int(prop(dtb, provider, '#clock-cells', 'x'), 16)
            result.append((provider, cells[:count]))
            del cells[:count]
        return result

    hdmitx = '/soc/bus@fe000000/hdmi-tx@300000'
    hdmirx = '/soc/bus@fe000000/hdmi-receiver@398000'
    assert prop(args.dtb, '/sound', 'simple-audio-card,name') == 'VIM4HDMIRX'
    assert prop(args.dtb, '/sound-hdmi-tx', 'model') == 'VIM4HDMITX'
    assert prop(args.dtb, hdmitx, '#sound-dai-cells', 'x') == '0'
    # Compare resolved clock providers, not unstable compiled phandle numbers.
    for name in ('clocks', 'assigned-clocks'):
        assert clock_spec(args.dtb, hdmirx, name) == clock_spec(args.baseline_dtb, hdmirx, name)
    for name in ('reg', 'assigned-clock-rates'):
        assert prop(args.dtb, hdmirx, name, 'x') == prop(args.baseline_dtb, hdmirx, name, 'x')
    assert prop(args.dtb, hdmirx, 'clock-names') == prop(args.baseline_dtb, hdmirx, 'clock-names')
    # Playback owns FRDDR_A, TDM_A and TOHDMITX; capture retains TODDR_A/HDMI RX.
    handles = phandles(args.dtb)
    playback = []
    for link in ('dai-link-0', 'dai-link-1', 'dai-link-2'):
        cells = prop(args.dtb, '/sound-hdmi-tx/' + link, 'sound-dai', 'x').split()
        playback.append(handles[int(cells[0], 16)])
    assert playback == ['/soc/bus@fe000000/audio-controller@3301c0',
                        '/audio-controller-0', '/soc/bus@fe000000/audio-controller@330744']
    rx_cpu = int(prop(args.dtb, '/sound/simple-audio-card,cpu', 'sound-dai', 'x'), 16)
    assert handles[rx_cpu] == '/soc/bus@fe000000/audio-controller@330100'
    for provider, _ in clock_spec(args.dtb, '/sound-hdmi-tx', 'assigned-clocks'):
        assert provider not in [entry[0] for entry in clock_spec(args.dtb, hdmirx, 'clocks')]
    print('Compiled audio DT: independent TX/RX cards, disjoint DMA/DAI paths and unchanged RX clocks')
