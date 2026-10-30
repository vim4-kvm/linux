#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Host tests execute the driver's bitmap IOVA allocator, including rollback."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).parent / 'drv/common/mm/adlak_mm_common.c').read_text()
init = source[source.index('int adlak_bitmap_pool_init('):
              source.index('void adlak_bitmap_dump(')]
allocate = source[source.index('unsigned long adlak_alloc_from_bitmap_pool('):
                  source.index('void adlak_set_bitmap_pool(')]
stubs = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#define AML_LOG_DEBUG(...) ((void)0)
#define AML_LOG_INFO(...) ((void)0)
#define AML_LOG_WARN(...) ((void)0)
#define ADLAK_MM_POOL_PAGE_SHIFT 12
#define ADLAK_MM_POOL_PAGE_SIZE 4096UL
#define ADLAK_MM_POOL_PAGE_MASK (~4095UL)
#define ADLAK_INVALID_ADDR (~0UL)
#define ADLAK_ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define ADLAK_GFP_KERNEL 0
#define BITS_TO_LONGS(v) (((v)+63)/64)
#define ERR(e) (-(e))
#define min(a,b) ((a)<(b)?(a):(b))
typedef unsigned long dma_addr_t;
struct adlak_mm_pool_priv { void *bitmap; size_t bitmap_size, addr_base, bits, used_count; int lock; };
struct adlak_mem_exclude { size_t start, size; };
static int fail_alloc, allocations;
static void *adlak_os_vzalloc(size_t n,int flags) {
 (void)flags; if(fail_alloc) return NULL; allocations++; return calloc(1,n);
}
static void adlak_os_vfree(void *p) { allocations--; free(p); }
static void adlak_os_mutex_init(int *p) { *p=0; }
static void adlak_os_mutex_destroy(int *p) { assert(!*p); }
static void adlak_os_mutex_lock(int *p) { assert(!*p); *p=1; }
static void adlak_os_mutex_unlock(int *p) { assert(*p); *p=0; }
static int bit(void *p,size_t n) { return (((unsigned long *)p)[n/64]>>(n%64))&1; }
static void bitmap_set(void *p,size_t n,size_t k) {
 for(size_t i=n;i<n+k;i++) ((unsigned long *)p)[i/64]|=1UL<<(i%64);
}
static void bitmap_clear(void *p,size_t n,size_t k) {
 for(size_t i=n;i<n+k;i++) ((unsigned long *)p)[i/64]&=~(1UL<<(i%64));
}
static size_t bitmap_find_next_zero_area(void *p,size_t bits,size_t start,size_t n,int mask) {
 (void)mask;
 for(size_t i=start;i+n<=bits;i++) {
  bool empty=true; for(size_t j=0;j<n;j++) if(bit(p,i+j)) empty=false;
  if(empty) return i;
 }
 return bits;
}
static void adlak_bitmap_dump(void *p) { (void)p; }
'''
test = r'''
int main(void) {
 struct adlak_mm_pool_priv pool={0};
 struct adlak_mem_exclude excluded={.start=30*4096,.size=20*4096};
 fail_alloc=1;
 assert(adlak_bitmap_pool_init(&pool,32*4096,&excluded)==-ENOMEM);
 assert(!allocations);
 fail_alloc=0;
 assert(!adlak_bitmap_pool_init(&pool,32*4096,&excluded));
 assert(adlak_alloc_from_bitmap_pool(&pool,0)==ADLAK_INVALID_ADDR);
 assert(adlak_alloc_from_bitmap_pool(&pool,SIZE_MAX)==ADLAK_INVALID_ADDR);
 unsigned long first=adlak_alloc_from_bitmap_pool(&pool,4097);
 unsigned long second=adlak_alloc_from_bitmap_pool(&pool,28*4096);
 assert(first==0 && second==8192 && pool.used_count==30);
 assert(adlak_alloc_from_bitmap_pool(&pool,4096)==ADLAK_INVALID_ADDR);
 adlak_free_to_bitmap_pool(&pool,first,4097);
 assert(pool.used_count==28);
 assert(adlak_alloc_from_bitmap_pool(&pool,4096)==0);
 assert(adlak_alloc_from_bitmap_pool(&pool,4096)==4096);
 adlak_free_to_bitmap_pool(&pool,0,8192);
 adlak_free_to_bitmap_pool(&pool,second,28*4096);
 assert(!pool.used_count);
 adlak_bitmap_pool_deinit(&pool);
 assert(!allocations);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(stubs + init + allocate + test)
    subprocess.run(['cc', '-std=gnu11', '-fsanitize=address,undefined',
                    str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
