"""Compile actual descriptor/store helpers to check per-function isolation."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('f_hid.c').read_text()


def function(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


intervals = function('static void hidg_set_intervals(', 'static int hidg_bind(')
store = function('static ssize_t f_hid_opts_interval_store(', 'CONFIGFS_ATTR(f_hid_opts_, interval);')
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
typedef uint8_t u8;
struct config_item { int unused; };
struct f_hid_opts { int lock, refcnt; u8 interval; bool interval_user_set; };
struct f_hidg { u8 interval; bool interval_user_set; };
static struct f_hid_opts options;
#define to_f_hid_opts(item) (&options)
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
static int kstrtouint(const char *s, int base, unsigned int *value) {
    char *end;
    unsigned long parsed;
    if (*s == '-') return -EINVAL;
    errno = 0;
    parsed = strtoul(s, &end, base);
    if (errno || end == s || (*end && *end != '\n') || parsed > UINT32_MAX)
        return -EINVAL;
    *value = parsed;
    return 0;
}
struct descriptor { u8 bInterval; };
static struct descriptor hidg_fs_in_ep_desc, hidg_fs_out_ep_desc;
static struct descriptor hidg_hs_in_ep_desc, hidg_hs_out_ep_desc;
static struct descriptor hidg_ss_in_ep_desc, hidg_ss_out_ep_desc;
'''
main = r'''
int main(void) {
    struct config_item item = {0};
    struct f_hidg keyboard = {.interval = 7, .interval_user_set = true};
    struct f_hidg mouse = {.interval = 4, .interval_user_set = true};
    struct f_hidg defaults = {0};
    hidg_set_intervals(&keyboard);
    assert(hidg_hs_in_ep_desc.bInterval == 7);
    hidg_set_intervals(&mouse);
    assert(hidg_hs_in_ep_desc.bInterval == 4);
    hidg_set_intervals(&keyboard);
    hidg_set_intervals(&defaults);
    assert(hidg_fs_in_ep_desc.bInterval == 10);
    assert(hidg_fs_out_ep_desc.bInterval == 10);
    assert(hidg_hs_in_ep_desc.bInterval == 4);
    assert(hidg_ss_out_ep_desc.bInterval == 4);
    assert(f_hid_opts_interval_store(&item, "7", 1) == 1);
    assert(options.interval == 7 && options.interval_user_set);
    options.refcnt = 1;
    assert(f_hid_opts_interval_store(&item, "4", 1) == -EBUSY);
    assert(options.interval == 7);
    options.refcnt = 0;
    assert(f_hid_opts_interval_store(&item, "256", 3) == -EINVAL);
    assert(f_hid_opts_interval_store(&item, "garbage", 7) == -EINVAL);
    assert(f_hid_opts_interval_store(&item, "-1", 2) == -EINVAL);
    assert(options.interval == 7);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture + intervals + store + main)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('HID intervals: independent functions, default reset, bound guard and invalid input passed')
