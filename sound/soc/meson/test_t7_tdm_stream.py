#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute stream rollback and trigger error propagation across formatters."""
from pathlib import Path
import subprocess
import tempfile

directory = Path(__file__).parent
source = (directory / 'axg-tdm-formatter.c').read_text()
start = source.index('int axg_tdm_stream_start')
function = source[start:source.index('EXPORT_SYMBOL_GPL(axg_tdm_stream_start)', start)]
source = (directory / 'axg-tdm-interface.c').read_text()
start = source.index('static int axg_tdm_iface_trigger')
trigger = source[start:source.index('static int axg_tdm_iface_remove_dai', start)]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#define guard(x) (void)
#define pr_err(...) ((void)0)
#define SNDRV_PCM_TRIGGER_START 1
#define SNDRV_PCM_TRIGGER_RESUME 2
#define SNDRV_PCM_TRIGGER_PAUSE_RELEASE 3
#define SNDRV_PCM_TRIGGER_STOP 4
#define SNDRV_PCM_TRIGGER_SUSPEND 5
#define SNDRV_PCM_TRIGGER_PAUSE_PUSH 6
struct axg_tdm_formatter { bool enabled, fail; struct axg_tdm_formatter *next; };
struct axg_tdm_stream { bool ready; int lock; struct { struct axg_tdm_formatter *first; } formatter_list; };
#define list_for_each_entry(f,h,l) for ((f) = (h)->first; (f); (f) = (f)->next)
static int axg_tdm_formatter_enable(struct axg_tdm_formatter *f) {
    if (f->fail) return -EIO;
    f->enabled = true;
    return 0;
}
static void axg_tdm_formatter_disable(struct axg_tdm_formatter *f) { f->enabled = false; }
static void axg_tdm_stream_stop(struct axg_tdm_stream *s) {
    struct axg_tdm_formatter *f;
    s->ready = false;
    list_for_each_entry(f, &s->formatter_list, list) axg_tdm_formatter_disable(f);
}
struct snd_pcm_substream { int unused; };
struct snd_soc_dai { struct axg_tdm_stream *stream; };
#define snd_soc_dai_get_dma_data(d,s) ((d)->stream)
'''
checks = r'''
int main(void) {
    struct axg_tdm_formatter formatters[3] = {{.next = &formatters[1]}, {.next = &formatters[2]}, {0}};
    struct axg_tdm_stream stream = {.formatter_list = {.first = &formatters[0]}};
    struct snd_soc_dai dai = {.stream = &stream};
    for (unsigned int fail = 0; fail < 3; fail++) {
        formatters[fail].fail = true;
        assert(axg_tdm_iface_trigger(NULL, SNDRV_PCM_TRIGGER_START, &dai) == -EIO);
        assert(!stream.ready);
        for (unsigned int i = 0; i < 3; i++) assert(!formatters[i].enabled);
        formatters[fail].fail = false;
        assert(!axg_tdm_iface_trigger(NULL, SNDRV_PCM_TRIGGER_START, &dai));
        assert(stream.ready);
        for (unsigned int i = 0; i < 3; i++) assert(formatters[i].enabled);
        assert(!axg_tdm_iface_trigger(NULL, SNDRV_PCM_TRIGGER_STOP, &dai));
    }
    assert(axg_tdm_iface_trigger(NULL, 99, &dai) == -EINVAL);
    puts("TDM multi-formatter rollback, successful retry and PCM trigger errors passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    code = Path(temporary) / 'stream.c'
    binary = Path(temporary) / 'stream'
    code.write_text(prefix + function + trigger + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
