#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run T7 link clock arithmetic against the vendor's 24/30-bit CTA tables."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', type=Path, required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
source = (here / 'meson_t7_modeset.c').read_text()
header = (here / 'meson_t7_modeset.h').read_text()
vendor = (args.vendor / 'drivers/media/vout/hdmitx21/hw/hw_clk.c').read_text()
types = header[header.index('struct meson_t7_clock {'):header.index('int meson_t7_link_clock')]
function = source[source.index('int meson_t7_link_clock_calculate('):
                  source.index('enum drm_mode_status meson_t7_mode_valid')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
#define min(a,b) ((a)<(b)?(a):(b))
#define DIV_ROUND_CLOSEST(a,b) (((a)+(b)/2)/(b))
'''
program += types + function + '\nint main(void) { struct meson_t7_link_clock l;\n'
rows = 0
for bpc, table in [(8, '24'), (10, '30')]:
    text = vendor.split('setting_enc_clk_val_' + table + '[] = {', 1)[1].split('\n};', 1)[0]
    for vic, pixel in [(4, 74250), (16, 148500), (95, 297000), (97, 594000)]:
        group, vco, od1, od2, od3, divider = next(
            row for row in re.findall(r'\{\{(.*?)},\s*(\d+), (\d+), (\d+), (\d+), '
                                       r'VID_PLL_DIV_(\w+),[^}]+}', text, re.S)
            if re.search(r'HDMI_' + str(vic) + '_', row[0]))
        for y420 in [False, True]:
            denominator = 16 if y420 else 8
            tmds = (pixel * bpc + denominator // 2) // denominator
            program += f'assert(meson_t7_link_clock_calculate({pixel},{bpc},{int(y420)},&l)'
            if tmds > 600000:
                program += '==-ERANGE);\n'
                continue
            program += '==0);\n'
            # The vendor has a separate 420 entry for 4K50/60 at 30 bits.
            # Restrict oracle rows to its actual CTA combinations.
            if y420 and vic != 97:
                continue
            expected_od1 = 2 if y420 and bpc == 8 else int(od1)
            program += (f'assert(l.pll.vco=={vco} && l.pll.od1*l.pll.od2=='
                        f'{expected_od1 * int(od2)} && l.od3=={1 if y420 else int(od3)});\n'
                        f'assert(l.tmds_khz=={tmds} && l.shift_sel=={3 if bpc == 10 else 2});\n')
            rows += 1
program += r'''
assert(meson_t7_link_clock_calculate(594000,10,true,&l)==0 && l.tmds_khz==371250);
assert(meson_t7_link_clock_calculate(148500,10,false,&l)==0 && l.tmds_khz==185625);
assert(meson_t7_link_clock_calculate(25174,8,false,&l)==-ERANGE);
assert(meson_t7_link_clock_calculate(148500,12,false,&l)==-ERANGE);
for (unsigned int p=25175; p<=594000; p+=997)
    for (unsigned int b=8; b<=10; b+=2)
        for (int y=0; y<2; y++)
            if (!meson_t7_link_clock_calculate(p,b,y,&l)) {
                unsigned long long serial=l.pll.vco*1000ULL/l.pll.od1/l.pll.od2/10;
                assert(llabs((long long)serial-l.tmds_khz*1000LL)<5000);
                assert(l.pixel_div==(y?2:1));
            }
puts("T7 link clocks: vendor CTA rows, deep colour/420 and computed-clock boundaries passed");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'clock.c').write_text(program)
    subprocess.run(['cc', '-Werror', str(path / 'clock.c'), '-o', str(path / 'clock')], check=True)
    subprocess.run([str(path / 'clock')], check=True)
print(f'{rows} vendor clock profiles compared')
