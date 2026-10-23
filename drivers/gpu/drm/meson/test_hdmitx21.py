#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the TX transport and DDC state machine with deterministic firmware replies."""
from pathlib import Path
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
static int queued, cancelled, notifications;
static enum drm_connector_status notified;
static int gpiod_get_value_cansleep(struct gpio_desc *gpio) { return gpio->level; }
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
