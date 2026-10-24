#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute MMIO callbacks against bounded banks with address sanitization."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).resolve().parents[3]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
static unsigned int readl(void *p) { return *(uint32_t *)p; }
static void writel(unsigned int value, void *p) { *(uint32_t *)p = value; }
'''
for path, function, start, core_size, extra, context in (
        ('drivers/clk/meson/axg-audio.c', 't7_audio', 'struct t7_audio_registers',
         0xf0, (0x100, 0x104),
         'struct t7_audio_registers banks = {.core = core, .pads = extra}; void *ctx = &banks;'),
        ('sound/soc/meson/t7-asrc.c', 't7_asrc', 'static int t7_asrc_read',
         0x108, (0x108,), 'void *banks[] = {core, extra}; void *ctx = banks;'),
        ('sound/soc/meson/t7-vad.c', 't7_vad', 'static int t7_vad_read',
         0xc8, (0xc8,), 'void *banks[] = {core, extra}; void *ctx = banks;')):
    text = (source / path).read_text()
    defines = '\n'.join(re.findall(r'^#define.*$', text, re.M)) if function != 't7_audio' else ''
    functions = text[text.index(start):text.index(f'static const struct regmap_bus {function}_')]
    checks = f'''
int main(void) {{
    uint32_t core[{core_size // 4}] = {{0}}, extra[{len(extra)}] = {{0}};
    {context}
    unsigned int value;
    for (unsigned int i = 0; i < sizeof(core) / 4; i++) {{
        assert(!{function}_write(ctx, i * 4, i + 1));
        assert(!{function}_read(ctx, i * 4, &value) && value == i + 1);
        assert(core[i] == i + 1 && extra[0] == 0);
    }}
'''
    for index, register in enumerate(extra):
        checks += f'''
    assert(!{function}_write(ctx, {register}, 0xa5a5 + {index}));
    assert(!{function}_read(ctx, {register}, &value) && value == 0xa5a5 + {index});
    assert(extra[{index}] == 0xa5a5 + {index});
    assert(core[0] == 1 && core[sizeof(core) / 4 - 1] == sizeof(core) / 4);
'''
    checks += f'puts("{function}: bounded core/secondary bank isolation passed");\n}}\n'
    with tempfile.TemporaryDirectory() as temporary:
        code = Path(temporary) / 'windows.c'
        binary = Path(temporary) / 'windows'
        code.write_text(prefix + defines + '\n' + functions + checks)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
