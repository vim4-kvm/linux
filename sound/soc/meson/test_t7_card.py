#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real backend fixups and FE negotiation across converter stages."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('axg-card.c').read_text()
start = source.index('static int axg_card_be_fixup')
functions = source[start:source.index('static int axg_card_add_link', start)]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define SNDRV_PCM_HW_PARAM_RATE 0
#define SNDRV_PCM_HW_PARAM_CHANNELS 1
struct snd_interval { unsigned int min, max; };
struct snd_pcm_hw_params { struct snd_interval values[2]; };
#define hw_param_interval(p,id) (&(p)->values[id])
struct axg_dai_link_tdm_data { unsigned int convert_rate, convert_channels; };
struct snd_soc_dai_link { bool dynamic, dpcm_merged_rate, dpcm_merged_chan; };
struct snd_soc_card { unsigned int num_links; struct snd_soc_dai_link *dai_link; void *data; };
struct snd_soc_pcm_runtime { unsigned int id; struct snd_soc_card *card; };
struct meson_card { struct axg_dai_link_tdm_data **link_data; };
#define snd_soc_card_get_drvdata(c) ((c)->data)
'''
checks = r'''
int main(void) {
    struct snd_soc_dai_link links[4] = {{.dynamic = true}, {0}, {0}, {.dynamic = true}};
    struct axg_dai_link_tdm_data pdm = {0}, reference = {0};
    struct axg_dai_link_tdm_data *data[] = {NULL, &pdm, &reference, NULL};
    struct meson_card private = {.link_data = data};
    struct snd_soc_card card = {.num_links = 4, .dai_link = links, .data = &private};
    struct snd_soc_pcm_runtime rtd = {.id = 1, .card = &card};
    struct snd_pcm_hw_params params = {.values = {{16000, 16000}, {6, 6}}};
    axg_card_update_frontends(&card);
    assert(links[0].dpcm_merged_rate && links[0].dpcm_merged_chan);
    assert(links[3].dpcm_merged_rate && links[3].dpcm_merged_chan);
    assert(!axg_card_be_fixup(&rtd, &params));
    assert(params.values[0].min == 16000 && params.values[1].min == 6);
    pdm.convert_channels = 4;
    reference.convert_channels = 2;
    reference.convert_rate = 48000;
    axg_card_update_frontends(&card);
    assert(!links[0].dpcm_merged_rate && !links[0].dpcm_merged_chan);
    assert(!links[3].dpcm_merged_rate && !links[3].dpcm_merged_chan);
    assert(!axg_card_be_fixup(&rtd, &params));
    assert(params.values[0].min == 16000 && params.values[0].max == 16000);
    assert(params.values[1].min == 4 && params.values[1].max == 4);
    rtd.id = 2;
    assert(!axg_card_be_fixup(&rtd, &params));
    assert(params.values[0].min == 48000 && params.values[0].max == 48000);
    assert(params.values[1].min == 2 && params.values[1].max == 2);
    assert(!links[1].dynamic && !links[2].dynamic);
    puts("Card unchanged legacy negotiation and 16k/6ch -> 16k/4ch + 48k/2ch fixups passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    code = Path(directory) / 'card.c'
    binary = Path(directory) / 'card'
    code.write_text(prefix + functions + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
