#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise vb2's base allocation contract through the driver's actual type."""
from pathlib import Path
import subprocess
import tempfile

text = Path(__file__).with_name('t7-vdin-test.c').read_text()
start = text.index('struct t7_vdin_buffer {')
end = text.index('struct t7_vdin_snapshot {', start)
declaration = text[start:end]
fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
typedef uint64_t u64, dma_addr_t;
struct vb2_buffer { unsigned index, state; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; unsigned sequence; };
struct list_head { void *next, *prev; };
struct dma_buf;
'''
checks = r'''
int main(void) {
 /* __vb2_queue_alloc assigns the start of kzalloc(buf_struct_size) to vb. */
 struct vb2_buffer *vb = calloc(1, sizeof(struct t7_vdin_buffer));
 assert(vb);
 vb->index = 13;
 struct vb2_v4l2_buffer *v4l2 = (void *)vb;
 struct t7_vdin_buffer *b = (void *)((char *)v4l2 -
                                  offsetof(struct t7_vdin_buffer, vb));
 assert((void *)b == (void *)vb);
 b->generation = 3; b->ticket = 613;
 vb->state = 2;
 assert(b->vb.vb2_buf.index == 13);
 assert(b->generation == 3 && b->ticket == 613);
 free(vb);
}
'''
with tempfile.TemporaryDirectory() as directory:
    code, binary = Path(directory)/'layout.c', Path(directory)/'layout'
    command = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', str(code), '-o', str(binary)]
    code.write_text(fixture + declaration + checks)
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    # Replay the actual a3 regression; the production assertion must reject it.
    broken = declaration.replace('\tstruct vb2_v4l2_buffer vb;\n\tu64 generation, ticket;',
                                 '\tu64 generation, ticket;\n\tstruct vb2_v4l2_buffer vb;')
    assert broken != declaration
    code.write_text(fixture + broken + checks)
    failure = subprocess.run(command, capture_output=True, text=True)
    assert failure.returncode and 'static assertion failed' in failure.stderr
print('vb2 base allocation: metadata preserved; misplaced prefix rejected: PASS')
