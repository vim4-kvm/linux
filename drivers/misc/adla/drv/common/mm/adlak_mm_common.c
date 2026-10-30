/*******************************************************************************
 * Copyright (C) 2022 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm_common.c
 * @brief
 *
 * <pre>
 * MODIFICATION HISTORY:
 *
 * Ver   	Who				Date				Changes
 * ----------------------------------------------------------------------------
 * 1.00a shiwei.sun@amlogic.com	2022/07/13	Initial release
 * </pre>
 *
 ******************************************************************************/

/***************************** Include Files *********************************/
#include "adlak_mm_common.h"

#include "adlak_mm_mbd.h"
#include "adlak_mm_os_common.h"
#include "adlak_mm_smmu.h"
/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Variable Definitions *****************************/
static struct adlak_simple_bitmap *ptr_mem_uid = NULL;

/************************** Function Prototypes ******************************/

int adlak_bitmap_pool_init(struct adlak_mm_pool_priv *pool, size_t pool_size,
                           struct adlak_mem_exclude *exclude_area) {
    size_t bitmap_size, start;
    size_t exclude_bitmap_count, exclude_area_start_aligned, exclude_area_size_aligned;
    AML_LOG_DEBUG("%s", __func__);
    bitmap_size = BITS_TO_LONGS(pool_size >> ADLAK_MM_POOL_PAGE_SHIFT) * sizeof(unsigned long);
    AML_LOG_DEBUG("%s pool_size 0x%lX bitmap_size 0x%lX", __func__, (uintptr_t)pool_size,
                  (uintptr_t)bitmap_size);

    pool->bitmap = adlak_os_vzalloc(bitmap_size, ADLAK_GFP_KERNEL);
    if (!pool->bitmap) {
        return ERR(ENOMEM);
    }
    pool->bitmap_size = bitmap_size;
    pool->addr_base   = 0;
    pool->used_count  = 0;
    pool->bits        = (pool_size >> ADLAK_MM_POOL_PAGE_SHIFT);

    // set the excluded area as invalid area
    if (exclude_area->size) {
        exclude_area_start_aligned = (exclude_area->start & ADLAK_MM_POOL_PAGE_MASK);
        exclude_area_size_aligned =
            ADLAK_ALIGN(exclude_area->start + exclude_area->size, ADLAK_MM_POOL_PAGE_SIZE) -
            exclude_area_start_aligned;

        exclude_area_start_aligned = exclude_area_start_aligned - pool->addr_base;
        exclude_bitmap_count       = exclude_area_size_aligned >> ADLAK_MM_POOL_PAGE_SHIFT;
        start                      = (exclude_area_start_aligned >> ADLAK_MM_POOL_PAGE_SHIFT);

        if (start < pool->bits) {
            AML_LOG_INFO("set exclude area in bitmap pool: start 0x%08lX, count 0x%08lX ",
                         (uintptr_t)(start), (uintptr_t)exclude_bitmap_count);
            bitmap_set(pool->bitmap, start, exclude_bitmap_count);
        }
    }

    AML_LOG_INFO(
        "mm_pool size = 0x%lX byte, bitmap_size = 0x%lX byte, bitmap_maxno = 0x%lX, "
        "pool_addr_base = 0x%lX.",
        (uintptr_t)pool_size, (uintptr_t)bitmap_size, (uintptr_t)pool->bits,
        (uintptr_t)pool->addr_base);
    adlak_os_mutex_init(&pool->lock);
    return 0;
}

void adlak_bitmap_pool_deinit(struct adlak_mm_pool_priv *pool) {
    adlak_os_mutex_lock(&pool->lock);
    adlak_os_mutex_unlock(&pool->lock);
    adlak_os_mutex_destroy(&pool->lock);
    if (pool->bitmap) {
        adlak_os_vfree(pool->bitmap);
        pool->bitmap = NULL;
    }
}

void adlak_bitmap_dump(struct adlak_mm_pool_priv *pool) {
#if ADLAK_DEBUG
    uint32_t bitmap_size = pool->bitmap_size;
    uint8_t *addr        = pool->bitmap;
    size_t   i;

    AML_LOG_WARN("%s", __func__);
    if (bitmap_size > (1024 * 1024 * 1024 / 4096 / BITS_PER_BYTE)) {
        //  bitmap_size = (1024 * 1024 * 1024 / 4096 / BITS_PER_BYTE);
    }
    for (i = bitmap_size - 1; i >= 15;) {
        AML_LOG_WARN(
            "offset %08lX ~ %08lX: %02X %02X %02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X %02X "
            "%02X %02X %02X %02X ",
            (uintptr_t)i, (uintptr_t)(i - 15), addr[i - 0], addr[i - 1], addr[i - 2], addr[i - 3],
            addr[i - 4], addr[i - 5], addr[i - 6], addr[i - 7], addr[i - 8], addr[i - 9],
            addr[i - 10], addr[i - 11], addr[i - 12], addr[i - 13], addr[i - 14], addr[i - 15]);
        if (i >= 16) {
            i -= 16;
        } else {
            i = 0;
        }
    }
#endif
}

unsigned long adlak_alloc_from_bitmap_pool(struct adlak_mm_pool_priv *pool, size_t size) {
    size_t        start = 0;
    size_t        bitmap_count;
    unsigned long addr_offset = ADLAK_INVALID_ADDR;
    if (!pool || size == 0) {
        return ERR(ENOMEM);
    }
    size         = ADLAK_ALIGN(size, ADLAK_MM_POOL_PAGE_SIZE);
    bitmap_count = size >> ADLAK_MM_POOL_PAGE_SHIFT;
    AML_LOG_DEBUG("%s 0x%lX byte", __func__, (uintptr_t)size);
    adlak_os_mutex_lock(&pool->lock);
    if (bitmap_count > (pool->bits - pool->used_count)) {
        AML_LOG_WARN(
            "Didn't find zero area from mem-pool!\nbitmap_maxno = %lu,bitmap_usedno = "
            "%lu,bitmap_count = %lu",
            (uintptr_t)pool->bits, (uintptr_t)pool->used_count, (uintptr_t)bitmap_count);
        goto ret;
    }
    start = bitmap_find_next_zero_area(pool->bitmap, pool->bits, 0, bitmap_count, 0);
    if (start >= pool->bits) {
        AML_LOG_WARN(
            "Didn't find zero area from mem-pool!\nbitmap_maxno = %lu,bitmap_usedno = "
            "%lu,bitmap_count = %lu",
            (uintptr_t)pool->bits, (uintptr_t)pool->used_count, (uintptr_t)bitmap_count);
        goto ret;
    }
    pool->used_count += bitmap_count;
    AML_LOG_DEBUG("start = %lu, bitmap_maxno = %lu,bitmap_usedno = %lu,bitmap_count = %lu",
                  (uintptr_t)start, (uintptr_t)pool->bits, (uintptr_t)pool->used_count,
                  (uintptr_t)bitmap_count);
    bitmap_set(pool->bitmap, start, bitmap_count);
    addr_offset = (unsigned long)(pool->addr_base + start * ADLAK_MM_POOL_PAGE_SIZE);
ret:
    if (ADLAK_INVALID_ADDR == addr_offset) {
        adlak_bitmap_dump(pool);
    }
    adlak_os_mutex_unlock(&pool->lock);

    return addr_offset;
}

void adlak_free_to_bitmap_pool(struct adlak_mm_pool_priv *pool, dma_addr_t addr_offset,
                               size_t size) {
    int bitmap_count;

    if (!pool) {
        return;
    }
    if (ADLAK_INVALID_ADDR == addr_offset) {
        return;
    }
    size         = ADLAK_ALIGN(size, ADLAK_MM_POOL_PAGE_SIZE);
    bitmap_count = size >> ADLAK_MM_POOL_PAGE_SHIFT;
    adlak_os_mutex_lock(&pool->lock);
    bitmap_clear(pool->bitmap, (addr_offset - pool->addr_base) / ADLAK_MM_POOL_PAGE_SIZE,
                 bitmap_count);
    pool->used_count -= bitmap_count;
    adlak_os_mutex_unlock(&pool->lock);
}

void adlak_set_bitmap_pool(struct adlak_mm_pool_priv *pool, dma_addr_t addr_offset, size_t size) {
    int bitmap_count;

    if (!pool) {
        return;
    }
    if (ADLAK_INVALID_ADDR == addr_offset) {
        return;
    }
    size         = ADLAK_ALIGN(size, ADLAK_MM_POOL_PAGE_SIZE);
    bitmap_count = size >> ADLAK_MM_POOL_PAGE_SHIFT;
    AML_LOG_INFO("%s %lu byte", __func__, (uintptr_t)size);
    adlak_os_mutex_lock(&pool->lock);
    bitmap_set(pool->bitmap, (addr_offset - pool->addr_base) / ADLAK_MM_POOL_PAGE_SIZE,
               bitmap_count);
    pool->used_count -= bitmap_count;
    adlak_os_mutex_unlock(&pool->lock);
}

/**
 * bitmap_find_next_zero_area - find a contiguous aligned zero area
 * @map: The address to base the search on
 * @bitmap_size: byte of bitmap
 * @bits: The bitmap size in bits
 * @offset: The bitnumber to start searching at
 * @nr: The number of zeroed bits we're looking for
 *
 */

signed long adlak_bitmap_find_next_zero_area_reverse(void *map, signed long pool_size,
                                                     signed long bits, signed long offset,
                                                     signed long nr) {
    signed long    consecutive_zeros       = 0;
    signed long    consecutive_zeros_start = -1;
    unsigned long  ulong_max               = -1;
    unsigned long *bitmap                  = (unsigned long *)map;
    signed long    i, j, index;
    signed long    bits_all     = pool_size * BITS_PER_BYTE;
    signed long    start_index  = bits_all;  // init as invalid value;
    signed long    bits_invalid = bits_all - bits;
    signed long    length       = pool_size / sizeof(long);
    signed long    start        = bits_all - (offset + bits_invalid) - 1;

    AML_LOG_INFO("pool size %lu, allocate %lu bits, start %lu\n", pool_size, nr, start);
    AML_LOG_DEBUG("ulong_max %016lX,BITS_PER_LONG %u\n", ulong_max, BITS_PER_LONG);
    // Iterate over the bitmap in units of long
    for (i = length - 1; i >= 0; --i) {
        AML_LOG_DEBUG("step 1: 0x%lX: 0x%016lX\n", i * 8, bitmap[i]);
        if (bitmap[i] == ulong_max) {
            consecutive_zeros       = 0;   // Reset consecutive zero count
            consecutive_zeros_start = -1;  /// Reset start index of consecutive zeros
            continue;
        }

        AML_LOG_DEBUG("step 2: 0x%lX: 0x%016lX\n", i * 8, bitmap[i]);
        // Iterate over each bit in the current long
        for (j = BITS_PER_LONG - 1; j >= 0; --j) {
            if (!(bitmap[i] & (1ul << j))) {
                index = i * BITS_PER_LONG + j;
                if (index <= start) {
                    consecutive_zeros++;
                    if (consecutive_zeros_start == -1) {
                        consecutive_zeros_start = index;
                    }
                }
            } else {
                consecutive_zeros       = 0;   // Reset consecutive zero count
                consecutive_zeros_start = -1;  // Reset start index of consecutive zeros
            }
            // If enough consecutive zeros are found, return the start index
            if (consecutive_zeros >= nr) {
                start_index = consecutive_zeros_start - nr + 1;
                goto end;
            }
        }
    }
end:
    if (start_index == bits_all) {
        AML_LOG_ERR("Insufficient contiguous free %lu bits\n", nr);
    }
#if 0
    else {
        if (start_index != bitmap_find_next_zero_area(map, bits, start_index, nr, 0)) {
            AML_LOG_ERR("alloc bitmap reverse double check fail");
            return bits_all;
        }
    }
#endif
    return start_index;
}

unsigned long adlak_alloc_from_bitmap_pool_reverse(struct adlak_mm_pool_priv *pool, size_t size) {
    size_t        start = 0;
    size_t        bitmap_count;
    unsigned long addr_offset = ADLAK_INVALID_ADDR;
    if (!pool || size == 0) {
        return ERR(ENOMEM);
    }
    size         = ADLAK_ALIGN(size, ADLAK_MM_POOL_PAGE_SIZE);
    bitmap_count = size >> ADLAK_MM_POOL_PAGE_SHIFT;

    AML_LOG_INFO("%s %lu byte", __func__, (uintptr_t)size);
    adlak_os_mutex_lock(&pool->lock);
    if (bitmap_count > (pool->bits - pool->used_count)) {
        AML_LOG_WARN(
            "Didn't find zero area from mem-pool!\nbitmap_maxno = %lu,bitmap_usedno = "
            "%lu,bitmap_count = %lu",
            (uintptr_t)pool->bits, (uintptr_t)pool->used_count, (uintptr_t)bitmap_count);
        goto ret;
    }
    start = adlak_bitmap_find_next_zero_area_reverse(pool->bitmap, pool->bitmap_size, pool->bits, 0,
                                                     bitmap_count);
    if (start >= pool->bits) {
        AML_LOG_WARN(
            "Didn't find zero area from mem-pool!\nbitmap_maxno = %lu,bitmap_usedno = "
            "%lu,bitmap_count = %lu",
            (uintptr_t)pool->bits, (uintptr_t)pool->used_count, (uintptr_t)bitmap_count);
        goto ret;
    }
    pool->used_count += bitmap_count;
    AML_LOG_INFO("start = %lu, bitmap_maxno = %lu,bitmap_usedno = %lu,bitmap_count = %lu",
                 (uintptr_t)start, (uintptr_t)pool->bits, (uintptr_t)pool->used_count,
                 (uintptr_t)bitmap_count);
    bitmap_set(pool->bitmap, start, bitmap_count);
    addr_offset = (unsigned long)(pool->addr_base + start * ADLAK_MM_POOL_PAGE_SIZE);
ret:
    if (ADLAK_INVALID_ADDR == addr_offset) {
        adlak_bitmap_dump(pool);
    }
    adlak_os_mutex_unlock(&pool->lock);

    return addr_offset;
}

int adlak_mem_uid_init(void) {
    int                      ret = 0;
    if (!ptr_mem_uid) {
        ptr_mem_uid = adlak_os_zalloc(sizeof(*ptr_mem_uid), ADLAK_GFP_KERNEL);
        if (unlikely(!ptr_mem_uid)) {
            ret = -1;
            goto end;
        }
        ptr_mem_uid->size = ADLAK_MEM_UID_SIZE;
        ret               = adlak_simple_bitmap_pool_init(ptr_mem_uid);
        if (unlikely(ret)) {
            ret = -1;
            goto end;
        } else {
            ret = 0;
        }
    }
end:
    if (ret) {
        ASSERT(0);
    }
    return ret;
    return 0;
}
void adlak_mem_uid_deinit(void) {
    if (ptr_mem_uid) {
        adlak_simple_bitmap_pool_deinit(ptr_mem_uid);
    }
}

uint64_t adlak_mem_uid_alloc(void) {
    uint64_t uid = ADLAK_INVALID_ADDR;
    int      id;
    if (ptr_mem_uid) {
        id = adlak_simple_bitmap_alloc(ptr_mem_uid);
        if (id >= 0) {
            uid = id << ADLAK_MM_POOL_PAGE_SHIFT;
        }
    }
    if (ADLAK_INVALID_ADDR == uid) {
        AML_LOG_ERR("alloc mem uid fail, you may need to increase the pool size of uid!");
        ASSERT(0);
    }
    return uid;
}
void adlak_mem_uid_free(uint64_t uid) {
    int id;
    // the net id info may stored in the lower 12 bits
    id = (uid >> ADLAK_MM_POOL_PAGE_SHIFT);
    adlak_simple_bitmap_free(ptr_mem_uid, id);
}

int adlak_simple_bitmap_pool_init(struct adlak_simple_bitmap *p_bitmap) {
    /* Allocate memory for the bitmap */
    uint32_t pool_size    = BITS_TO_LONGS(p_bitmap->size) * sizeof(unsigned long);
    p_bitmap->bitmap_pool = adlak_os_malloc(pool_size, ADLAK_GFP_KERNEL);
    if (!p_bitmap->bitmap_pool) {
        AML_LOG_ERR("Failed to allocate memory for bitmap\n");
        return -ENOMEM;
    }
    /* Initialize the bitmap */
    bitmap_zero(p_bitmap->bitmap_pool, p_bitmap->size);
    p_bitmap->rpt = 0;

    return 0;
}

void adlak_simple_bitmap_pool_deinit(struct adlak_simple_bitmap *p_bitmap) {
    if (p_bitmap->bitmap_pool) {
        adlak_os_free(p_bitmap->bitmap_pool);
        p_bitmap->bitmap_pool = NULL;
    }
}

int adlak_simple_bitmap_alloc(struct adlak_simple_bitmap *p_bitmap) {
    int id;
    {
        /* Find the first zero bit and set it */
        id = find_first_zero_bit(p_bitmap->bitmap_pool, p_bitmap->size);
        if (id >= p_bitmap->size) {
            AML_LOG_ERR("No available bit in bitmap\n");
            return -1; /* No available ID */
        }
    }
    p_bitmap->rpt = id+1;
    /* Set the bit to mark the ID as used */
    set_bit(id, p_bitmap->bitmap_pool);
    return id;
}

void adlak_simple_bitmap_free(struct adlak_simple_bitmap *p_bitmap, int id) {
    if (id < 0 || id >= p_bitmap->size) {
        AML_LOG_ERR("Invalid ID to free: %d\n", id);
        return;
    }
    /* Clear the bit to mark the ID as free */
    clear_bit(id, p_bitmap->bitmap_pool);
}