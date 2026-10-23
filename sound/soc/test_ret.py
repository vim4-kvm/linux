#!/usr/bin/env python3
"""Exercise the actual ASoC return handler across source loss and device faults."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('soc-utils.c').read_text()
function = source[source.index('int snd_soc_ret('):source.index('EXPORT_SYMBOL_GPL(snd_soc_ret);')]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#define EPROBE_DEFER 517
#define ENOTSUPP 524
struct device { int unused; };
struct va_format { const char *fmt; va_list *va; };
static int errors, debug;
static void dev_err(const struct device *dev, const char *fmt, ...)
{ (void)dev; (void)fmt; errors++; }
static void dev_dbg(const struct device *dev, const char *fmt, ...)
{ (void)dev; (void)fmt; debug++; }
''' + function + r'''
int main(void)
{
    struct device dev = { 0 };
    int faults[] = { -EIO, -EINVAL, -ENODEV, -ERANGE };
    unsigned int i;

    /* Repeated unplug/retry preserves errno without flooding error logs. */
    for (i = 0; i < 100; i++)
        assert(snd_soc_ret(&dev, -ENOLINK, "hw_params %d", i) == -ENOLINK);
    assert(errors == 0 && debug == 100);
    /* Qualification can race a changing ACR/AIF snapshot even with TMDS up. */
    for (i = 0; i < 100; i++)
        assert(snd_soc_ret(&dev, -EAGAIN, "hw_params %d", i) == -EAGAIN);
    assert(errors == 0 && debug == 200);
    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++)
        assert(snd_soc_ret(&dev, faults[i], "device fault") == faults[i]);
    assert(errors == 4 && debug == 200);
    assert(snd_soc_ret(&dev, 0, "success") == 0);
    assert(snd_soc_ret(&dev, 7, "positive result") == 7);
    assert(snd_soc_ret(&dev, -EPROBE_DEFER, "probe") == -EPROBE_DEFER);
    assert(snd_soc_ret(&dev, -ENOTSUPP, "optional") == -ENOTSUPP);
    assert(snd_soc_ret(&dev, -EOPNOTSUPP, "optional") == -EOPNOTSUPP);
    assert(errors == 4 && debug == 200);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'test.c').write_text(program)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(root / 'test.c'),
                    '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
print('ASOC_RETRY_RETURN_AND_DIAGNOSTICS_PASS')
