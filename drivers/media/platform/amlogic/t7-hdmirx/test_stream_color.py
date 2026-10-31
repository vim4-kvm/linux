#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual stream metadata function against a locked-state fixture."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-hdmirx.c').read_text()
start = source.index('int t7_hdmirx_get_stream_color(')
function = source[start:source.index('\nEXPORT_SYMBOL_GPL', start)]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
struct link { int lock, users; };
struct t7_hdmirx { struct link *link; bool dynamic_modes, lease_hdr;
 struct {unsigned depth,space,colorimetry; bool full;} lease_color; };
struct device { struct t7_hdmirx *rx; };
struct t7_hdmirx_stream_color { unsigned depth,space,colorimetry; bool full_range,hdr10; };
static void *dev_get_drvdata(struct device *d) {return d->rx;}
static void mutex_lock(int *m) {assert(!*m); *m=1;}
static void mutex_unlock(int *m) {assert(*m); *m=0;}
/* FUNCTION */
int main(void) {
 struct link link={0}; struct t7_hdmirx rx={.link=&link}; struct device dev={&rx};
 struct t7_hdmirx_stream_color out={.depth=99};
 assert(t7_hdmirx_get_stream_color(&dev,0)==-EINVAL && !link.lock);
 assert(t7_hdmirx_get_stream_color(&dev,&out)==-ENODATA && out.depth==99 && !link.lock);
 link.users=1;
 assert(t7_hdmirx_get_stream_color(&dev,&out)==-EOPNOTSUPP && out.depth==99 && !link.lock);
 rx.dynamic_modes=true; rx.lease_hdr=true; rx.lease_color.depth=10;
 rx.lease_color.space=3; rx.lease_color.colorimetry=2; rx.lease_color.full=true;
 assert(!t7_hdmirx_get_stream_color(&dev,&out));
 assert(out.depth==10 && out.space==3 && out.colorimetry==2 && out.hdr10 && out.full_range && !link.lock);
}
'''
with tempfile.TemporaryDirectory() as temp:
    c = Path(temp) / 'test.c'
    c.write_text(fixture.replace('/* FUNCTION */', function))
    binary = Path(temp) / 'test'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('stream metadata admission and lease snapshot: PASS')
