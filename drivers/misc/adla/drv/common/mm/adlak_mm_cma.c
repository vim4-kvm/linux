/*******************************************************************************
 * Copyright (C) 2024 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm_cma.c
 * @brief
 *
 * <pre>
 * MODIFICATION HISTORY:
 *
 * Ver   	Who				Date				Changes
 * ----------------------------------------------------------------------------
 * 1.00a shiwei.sun@amlogic.com	2024/03/13	Initial release
 * </pre>
 *
 ******************************************************************************/

/***************************** Include Files *********************************/
#include "adlak_mm_cma.h"

#include "adlak_mm_common.h"
#include "adlak_mm_os_common.h"

/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

struct adlak_mem_cma {
    struct adlak_mem_usage      usage;
    adlak_os_mutex_t            mutex;
    struct adlak_device *       padlak;
    struct device *             dev;
    struct adlak_mem            mm;
    uint8_t                     has_mem_pool;
    struct adlak_mem_pool_info *mem_pool;

    /*Share swap memory between different models to avoid dram size usage,
    but sacrifices performance when executing multiple models simultaneously*/
    struct adlak_share_swap_buf share_buf;
};

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Variable Definitions *****************************/

static struct adlak_mem_cma *ptr_mm_cma = NULL;
/************************** Function Prototypes ******************************/

#if CONFIG_ADLAK_MEM_POOL_EN
static int adlak_mem_pools_mgr_init(struct adlak_mem_exclude *exclude_area);

static void adlak_mem_pools_mgr_deinit(void);

static int adlak_mem_pools_mgr_init(struct adlak_mem_exclude *exclude_area) {
    size_t pool_size = ptr_mm_cma->mem_pool->size;
    AML_LOG_DEBUG("%s", __func__);

    ptr_mm_cma->usage.pool_size += pool_size;
    return adlak_bitmap_pool_init(&ptr_mm_cma->mem_pool->bitmap_area, pool_size, exclude_area);
}

static void adlak_mem_pools_mgr_deinit(void) {
    adlak_bitmap_pool_deinit(&ptr_mm_cma->mem_pool->bitmap_area);
}

/*Reverse lookup of a region from the bitmap pool,
 */
static unsigned long adlak_alloc_share_buf_from_bitmap_pool_reverse(struct adlak_mem *mm,
                                                                    size_t            size) {
    size_t                       start = 0, search_start;
    size_t                       bitmap_count;
    unsigned long                addr_offset = ADLAK_INVALID_ADDR;
    struct adlak_mm_pool_priv *  pool        = &ptr_mm_cma->mem_pool->bitmap_area;
    struct adlak_share_swap_buf *share_buf   = &ptr_mm_cma->share_buf;

    AML_LOG_DEBUG("%s\n", __func__);
    if (!pool || size == 0) {
        return ERR(ENOMEM);
    }
    size         = ADLAK_ALIGN(size, ADLAK_MM_POOL_PAGE_SIZE);
    bitmap_count = size >> ADLAK_MM_POOL_PAGE_SHIFT;

    AML_LOG_DEBUG("[share mem]need  bitmap_count %lu", (uintptr_t)bitmap_count);
    // get max value
    bitmap_count = ((bitmap_count) < (share_buf->bitmap_count_max) ? (share_buf->bitmap_count_max)
                                                                   : (bitmap_count));
    if ((bitmap_count - share_buf->bitmap_count_max) > (pool->bits - pool->used_count)) {
        AML_LOG_WARN(
            "Didn't find zero area from mem-pool!\nbitmap_maxno = %lu,bitmap_usedno = "
            "%lu,bitmap_count = %lu",
            (uintptr_t)pool->bits, (uintptr_t)pool->used_count,
            (uintptr_t)(bitmap_count - share_buf->bitmap_count_max));
        goto ret;
    }

    if (share_buf->bitmap_count_max) {
        /* Shared space is already taken by another context*/
        AML_LOG_DEBUG("[share mem]clear bitmap from %lu,bitmap_count %lu\n",
                      (uintptr_t)share_buf->offset_start, (uintptr_t)share_buf->bitmap_count_max);
        bitmap_clear(pool->bitmap, share_buf->offset_start, share_buf->bitmap_count_max);
    }
    AML_LOG_DEBUG("[share mem]real alloc  bitmap_count %lu", (uintptr_t)bitmap_count);

    search_start = pool->bits - bitmap_count;
    start = bitmap_find_next_zero_area(pool->bitmap, pool->bits, search_start, bitmap_count, 0);

    if (start > pool->bits) {
        AML_LOG_WARN(
            "Didn't find zero area from mem-pool!\nbitmap_maxno = %lu,bitmap_usedno = "
            "%lu,bitmap_count = %lu",
            (uintptr_t)pool->bits, (uintptr_t)pool->used_count, (uintptr_t)bitmap_count);
        goto ret;
    }

    if (share_buf->offset_start < pool->bits) {
        pool->used_count -= share_buf->bitmap_count_max;
    }
    share_buf->bitmap_count_max = bitmap_count;  // update the share_buf->bitmap_count
    pool->used_count += share_buf->bitmap_count_max;

    share_buf->offset_start = start;

    AML_LOG_DEBUG("start = %lu, bitmap_maxno = %lu,bitmap_usedno = %lu,bitmap_count = %lu",
                  (uintptr_t)start, (uintptr_t)pool->bits, (uintptr_t)pool->used_count,
                  (uintptr_t)bitmap_count);
    bitmap_set(pool->bitmap, start, bitmap_count);
    addr_offset = (unsigned long)(pool->addr_base + start * ADLAK_MM_POOL_PAGE_SIZE);

    share_buf->ref_cnt++;
    share_buf->share_buf_size = bitmap_count * ADLAK_MM_POOL_PAGE_SIZE;

    AML_LOG_INFO("[share mem]addr_offset 0x%08lX", (uintptr_t)addr_offset);
ret:
    if (ADLAK_INVALID_ADDR == addr_offset) {
        adlak_bitmap_dump(pool);
    }

    return addr_offset;
}

static void adlak_free_to_bitmap_pool_reverse(struct adlak_mem *mm) {
    struct adlak_mm_pool_priv *  pool      = &ptr_mm_cma->mem_pool->bitmap_area;
    struct adlak_share_swap_buf *share_buf = &ptr_mm_cma->share_buf;
    int                          bitmap_count;
    int                          ref_cnt;

    if (!pool || share_buf->ref_cnt == 0) {
        return;
    }
    AML_LOG_DEBUG("%s\n", __func__);
    share_buf->ref_cnt--;
    ref_cnt = share_buf->ref_cnt;
    if (0 > ref_cnt) {
        AML_LOG_ERR("%s, ref_cnt[%d] less than zero!\n", __func__, ref_cnt);
    }
    if (0 == ref_cnt) {
        // TODO  Avoid counting errors caused by double free,so check the net_id
        AML_LOG_INFO("[share mem] free share buf");
        if (share_buf->bitmap_count_max) {
            bitmap_count = share_buf->bitmap_count_max;
            bitmap_clear(pool->bitmap, share_buf->offset_start, bitmap_count);
            pool->used_count -= bitmap_count;
            share_buf->bitmap_count_max = 0;
            share_buf->offset_start     = -1;
            share_buf->share_buf_size   = 0;
        }
    }
}

static int adlak_malloc_from_mem_pool(struct adlak_mem *mm, struct adlak_mem_handle *mm_info) {
    unsigned long start;
    AML_LOG_DEBUG("%s", __func__);
    if (!(mm_info->req.mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE)) {
        start = adlak_alloc_from_bitmap_pool(&ptr_mm_cma->mem_pool->bitmap_area,
                                             (size_t)mm_info->req.bytes);
    } else {
        start = adlak_alloc_share_buf_from_bitmap_pool_reverse(mm, (size_t)mm_info->req.bytes);
    }

    if (ADLAK_IS_ERR((void *)start)) {
        mm_info->phys_addr = 0;
        return ERR(ENOMEM);
    }

    mm_info->from_pool = 1;
    mm_info->mem_src   = ptr_mm_cma->mem_pool->mem_src;
    mm_info->cpu_addr =
        (void *)((dma_addr_t)ptr_mm_cma->mem_pool->cpu_addr_base + (dma_addr_t)start);
    mm_info->dma_addr  = ptr_mm_cma->mem_pool->dma_addr_base + (dma_addr_t)start;
    mm_info->phys_addr = ptr_mm_cma->mem_pool->phys_addr_base + (phys_addr_t)start;

    mm_info->iova_addr = mm_info->phys_addr;

    return ERR(NONE);
}

static void adlak_free_to_mem_pool(struct adlak_mem *mm, struct adlak_mem_handle *mm_info) {
    unsigned long start;
    AML_LOG_DEBUG("%s", __func__);
    if (mm_info->phys_addr) {
        start = mm_info->phys_addr - ptr_mm_cma->mem_pool->phys_addr_base;
        if (!(mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE)) {
            adlak_free_to_bitmap_pool(&ptr_mm_cma->mem_pool->bitmap_area, start,
                                      (size_t)mm_info->req.bytes);
        } else {
            adlak_free_to_bitmap_pool_reverse(mm);
        }
    }
    mm_info->phys_addr = 0;
}

#ifndef CONFIG_ADLA_FREERTOS
static void adlak_free_cma_region_nocache(void) {
    if (ADLAK_IS_ERR_OR_NULL(ptr_mm_cma->mem_pool)) {
        return;
    }
    if (ptr_mm_cma->mem_pool->cpu_addr_base) {
        dma_free_coherent(ptr_mm_cma->dev, ptr_mm_cma->mem_pool->size,
                          ptr_mm_cma->mem_pool->cpu_addr_base, ptr_mm_cma->mem_pool->dma_addr_base);
    }
    adlak_os_free(ptr_mm_cma->mem_pool);
    ptr_mm_cma->mem_pool = NULL;
}

static int adlak_alloc_cma_region_nocache(void) {
    int ret;

    dma_addr_t dma_hd = 0;
    uint64_t   size;
    void *     vaddr = NULL;
    int        try;
    size_t     size_dec;
    AML_LOG_DEBUG("%s", __func__);
    //
    ret = adlak_platform_get_rsv_mem_size(ptr_mm_cma->dev, &size);
    if (ret) {
        goto err;
    }
    try      = 10;
    size_dec = size / 16;
    while (try--) {
        vaddr = dma_alloc_coherent(ptr_mm_cma->dev, (size_t)size, &dma_hd, ADLAK_GFP_KERNEL);
        if (!vaddr) {
            AML_LOG_ERR("DMA alloc coherent failed: pa 0x%lX, size = %lu\n", (uintptr_t)dma_hd,
                        (uintptr_t)size);
            size = size - size_dec;
        } else {
            break;
        }
    }
    if (!vaddr) {
        goto err;
    }

    ptr_mm_cma->mem_pool = adlak_os_zalloc(sizeof(struct adlak_mem_pool_info), ADLAK_GFP_KERNEL);
    if (ADLAK_IS_ERR_OR_NULL(ptr_mm_cma->mem_pool)) {
        goto err_alloc;
    }

    ptr_mm_cma->mem_pool->cpu_addr_base  = vaddr;
    ptr_mm_cma->mem_pool->phys_addr_base = dma_to_phys(ptr_mm_cma->dev, dma_hd);
    ptr_mm_cma->mem_pool->dma_addr_base  = dma_hd;
    ptr_mm_cma->mem_pool->size           = (size_t)size;
    ptr_mm_cma->mem_pool->mem_src        = ADLAK_ENUM_MEMSRC_CMA;
    ptr_mm_cma->mem_pool->cacheable      = false;

    AML_LOG_INFO("cma memory info: dma_addr= 0x%lX,  phys_addr= 0x%lX,size=%lu MByte\n",
                 (uintptr_t)ptr_mm_cma->mem_pool->dma_addr_base,
                 (uintptr_t)ptr_mm_cma->mem_pool->phys_addr_base,
                 (uintptr_t)(ptr_mm_cma->mem_pool->size / (1024 * 1024)));

    return 0;
err_alloc:
    dma_free_coherent(ptr_mm_cma->dev, size, vaddr, dma_hd);
err:
    adlak_free_cma_region_nocache();
    return (ERR(ENOMEM));
}
#endif

#endif

static void adlak_free_share_through_dma(struct adlak_mem *mm, struct adlak_mem_handle *mm_info) {
    if (mm_info->cpu_addr) {
        ptr_mm_cma->share_buf.ref_cnt--;
        adlak_os_printf(
            "%s: dma_addr= 0x%lX,  phys_addr= "
            "0x%lX,ref_cnt=%lu\n",
            __FUNCTION__, (uintptr_t)mm_info->dma_addr, (uintptr_t)mm_info->phys_addr,
            (uintptr_t)(ptr_mm_cma->share_buf.ref_cnt));
        if (0 == ptr_mm_cma->share_buf.ref_cnt) {
            adlak_os_printf("dma_free_coherent: cpu_addr= 0x%lX\n", (uintptr_t)mm_info->cpu_addr);
            adlak_free_through_dma(mm, mm_info);
            ptr_mm_cma->share_buf.share_buf_cpu_addr  = 0;
            ptr_mm_cma->share_buf.share_buf_dma_addr  = 0;
            ptr_mm_cma->share_buf.share_buf_phys_addr = 0;
        }
        mm_info->cpu_addr = NULL;
    }
}

static int adlak_malloc_share_through_dma(struct adlak_mem *mm, struct adlak_mem_handle *mm_info) {
    int ret;
    // gfp_t gfp = 0;
    if (!ptr_mm_cma->share_buf.share_buf_cpu_addr) {
        mm_info->req.bytes = (size_t)ptr_mm_cma->share_buf.share_buf_size;
        ret                = adlak_malloc_through_dma(&ptr_mm_cma->mm, mm_info);
        if (ERR(NONE) != ret) {
            return ret;
        }

        ptr_mm_cma->share_buf.share_buf_cpu_addr  = mm_info->cpu_addr;
        ptr_mm_cma->share_buf.share_buf_dma_addr  = mm_info->dma_addr;
        ptr_mm_cma->share_buf.share_buf_phys_addr = mm_info->phys_addr;
    }

    mm_info->req.bytes = (size_t)ptr_mm_cma->share_buf.share_buf_size;
    mm_info->cpu_addr  = ptr_mm_cma->share_buf.share_buf_cpu_addr;
    mm_info->dma_addr  = ptr_mm_cma->share_buf.share_buf_dma_addr;
    mm_info->phys_addr = ptr_mm_cma->share_buf.share_buf_phys_addr;

    mm_info->mem_src   = ADLAK_ENUM_MEMSRC_CMA;
    mm_info->iova_addr = mm_info->phys_addr;
    ptr_mm_cma->share_buf.ref_cnt++;
    adlak_os_printf(
        "%s: dma_addr= 0x%lX,  phys_addr= "
        "0x%lX,size=%lu KByte, ref_cnt=%lu\n",
        __FUNCTION__, (uintptr_t)mm_info->dma_addr, (uintptr_t)mm_info->phys_addr,
        (uintptr_t)(mm_info->req.bytes / 1024), (uintptr_t)(ptr_mm_cma->share_buf.ref_cnt));

    return ERR(NONE);
}

static void inline adlak_cma_claim_dev(void) {
    AML_LOG_DEBUG("%s[+]", __func__);
    adlak_os_mutex_lock(&ptr_mm_cma->mutex);
    AML_LOG_DEBUG("%s[-]", __func__);
}

static void inline adlak_cma_release_dev(void) {
    AML_LOG_DEBUG("%s[+]", __func__);
    adlak_os_mutex_unlock(&ptr_mm_cma->mutex);
    AML_LOG_DEBUG("%s[-]", __func__);
}

static void adlak_cma_rewrite_memtype(struct adlak_buf_req *   pbuf_req,
                                      struct adlak_mem_handle *mm_info) {
    mm_info->req.mem_type = 0;

    if (!(pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_INNER)) {
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS;
    } else {
        if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_CONTIGUOUS) {
            mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS;
        }
        if (0 != ptr_mm_cma->share_buf.share_swap_en) {
            if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_SHARE) {
                /*share memory between different model only supports soc without smmu for the time
                 * being*/
                mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_SHARE;
            }
        }
    }

    {
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS;
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_PA_WITHIN_4G;
    }

    if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_PA_WITHIN_4G) {
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_PA_WITHIN_4G;
    }

    mm_info->mem_type = mm_info->req.mem_type;
}

static int adlak_cma_cache_flush(struct adlak_mem_handle *         mm_info,
                                 struct adlak_sync_cache_ext_info *sync_cache_ext_info) {
    AML_LOG_DEBUG("%s", __func__);
    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        return -1;
    }
    if (mm_info->mem_src == ADLAK_ENUM_MEMSRC_OS || mm_info->mem_src == ADLAK_ENUM_MEMSRC_CMA ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_RESERVED ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_EXT_PHYS) {
        adlak_os_flush_cache(&ptr_mm_cma->mm, mm_info, sync_cache_ext_info);
    }
    return 0;
}

static int adlak_cma_cache_invalid(struct adlak_mem_handle *         mm_info,
                                   struct adlak_sync_cache_ext_info *sync_cache_ext_info) {
    AML_LOG_DEBUG("%s", __func__);
    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        return -1;
    }
    if (mm_info->mem_src == ADLAK_ENUM_MEMSRC_OS || mm_info->mem_src == ADLAK_ENUM_MEMSRC_CMA ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_RESERVED ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_EXT_PHYS) {
        adlak_os_invalid_cache(&ptr_mm_cma->mm, mm_info, sync_cache_ext_info);
    }
    return 0;
}

static void adlak_cma_free(struct adlak_context_smmu_attr *ctx_smmu_attr,
                           struct adlak_mem_handle *       mm_info) {
    ASSERT(mm_info);
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    if (NULL == ctx_smmu_attr) {
        ptr_mm_cma->usage.alloced_kmd -= mm_info->req.bytes;
    } else {
        if (0 == (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE)) {
            ptr_mm_cma->usage.alloced_umd -= mm_info->req.bytes;
            ctx_smmu_attr->alloc_byte -= mm_info->req.bytes;
        }
    }

    if (ptr_mm_cma->has_mem_pool) {
        // free uncacheable memory from memory pool
        if (mm_info->from_pool) {
            adlak_free_to_mem_pool(&ptr_mm_cma->mm, mm_info);
        } else {
            adlak_free_through_dma(&ptr_mm_cma->mm, mm_info);
        }
    } else {
        if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE) {
            adlak_free_share_through_dma(&ptr_mm_cma->mm, mm_info);
        } else {
            // free uncacheable memory through dma api
            adlak_free_through_dma(&ptr_mm_cma->mm, mm_info);
        }
    }
    adlak_mem_uid_free(mm_info->uid);
    adlak_os_free(mm_info);
    adlak_cma_release_dev();
}

static struct adlak_mem_handle *adlak_cma_alloc(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                                struct adlak_buf_req *          pbuf_req) {
    int                      ret = ERR(ENOMEM);
    struct adlak_mem_handle *mm_info;
    if (!pbuf_req->bytes) {
        return (void *)NULL;
    }
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    mm_info = adlak_os_zalloc(sizeof(struct adlak_mem_handle), ADLAK_GFP_KERNEL);
    if (unlikely(!mm_info)) {
        goto end;
    }
    mm_info->uid               = adlak_mem_uid_alloc();
    mm_info->iova_addr         = ADLAK_INVALID_ADDR;  // init as invalid value
    mm_info->req.bytes         = ADLAK_PAGE_ALIGN(pbuf_req->bytes);
    mm_info->req.mem_direction = pbuf_req->mem_direction;

    adlak_cma_rewrite_memtype(pbuf_req, mm_info);

    mm_info->nr_pages = ADLAK_DIV_ROUND_UP(mm_info->req.bytes, ADLAK_PAGE_SIZE);
    mm_info->cpu_addr = NULL;

    // 1. alloc + mmap
    if (ptr_mm_cma->has_mem_pool) {
        // alloc memory from memory pool
        ret = adlak_malloc_from_mem_pool(&ptr_mm_cma->mm, mm_info);
        if (ERR(NONE) != ret) {
            if (mm_info->req.mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE) {
                goto end;
            }
            // alloc memory through dma api
            ret = adlak_malloc_through_dma(&ptr_mm_cma->mm, mm_info);
        } else {
            goto end;
        }
    } else {
        if (mm_info->req.mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE) {
            ret = adlak_malloc_share_through_dma(&ptr_mm_cma->mm, mm_info);
            goto end;
        } else {
            // alloc memory through dma api
            ret = adlak_malloc_through_dma(&ptr_mm_cma->mm, mm_info);
            goto end;
        }
    }

end:

    if (ret) {
        AML_LOG_ERR("%s fail!", __FUNCTION__);
    } else {
        mm_info->iova_addr = mm_info->phys_addr;
        if (NULL == ctx_smmu_attr) {
            ptr_mm_cma->usage.alloced_kmd += mm_info->req.bytes;
        } else {
            if (0 == (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE)) {
                ptr_mm_cma->usage.alloced_umd += mm_info->req.bytes;
                ctx_smmu_attr->alloc_byte += mm_info->req.bytes;
            }
        }
    }
    adlak_cma_release_dev();
    return mm_info;
}

static void adlak_cma_dettach(struct adlak_context_smmu_attr *ctx_smmu_attr,
                              struct adlak_mem_handle *       mm_info) {
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    adlak_os_dettach_ext_mem(&ptr_mm_cma->mm, mm_info);

    adlak_mem_uid_free(mm_info->uid);
    adlak_os_free(mm_info);
    adlak_cma_release_dev();
}

static struct adlak_mem_handle *adlak_cma_attach(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                                 struct adlak_extern_buf_info *  pbuf_req) {
    int                      ret;
    struct adlak_mem_handle *mm_info   = NULL;
    uint64_t                 phys_addr = pbuf_req->buf_handle;
    size_t                   size      = pbuf_req->bytes;
    if (!size || !ADLAK_IS_ALIGNED((unsigned long)(size), ADLAK_PAGE_SIZE)) {
        goto early_exit;
    }

    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    mm_info = adlak_os_zalloc(sizeof(struct adlak_mem_handle), ADLAK_GFP_KERNEL);
    if (unlikely(!mm_info)) {
        goto end;
    }
    mm_info->uid          = adlak_mem_uid_alloc();
    mm_info->iova_addr    = ADLAK_INVALID_ADDR;  // init as invalid value
    mm_info->req.bytes    = size;
    mm_info->req.mem_type = ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS;
    // set as uncacheable for exttern memory
    mm_info->req.mem_direction = pbuf_req->mem_direction;

    mm_info->nr_pages = ADLAK_DIV_ROUND_UP(mm_info->req.bytes, ADLAK_PAGE_SIZE);
    mm_info->cpu_addr = NULL;

    mm_info->mem_src  = ADLAK_ENUM_MEMSRC_EXT_PHYS;
    mm_info->mem_type = mm_info->req.mem_type;
    ret               = adlak_os_attach_ext_mem(&ptr_mm_cma->mm, mm_info, phys_addr);

    if (!ret) {
        pbuf_req->errcode = 0;
    } else {
        pbuf_req->errcode = ret;
        adlak_mem_uid_free(mm_info->uid);
        adlak_os_free(mm_info);
        mm_info = (void *)NULL;
        goto end;
    }

    mm_info->iova_addr = mm_info->phys_addr;

end:
    adlak_cma_release_dev();
early_exit:
    return mm_info;
}

static int adlak_cma_unmap(struct adlak_mem_handle *mm_info) {
    // nothing todo
    return 0;
}

static int adlak_cma_mmap(struct adlak_mem_handle *mm_info, void *const vma) {
    int ret;
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    ret = adlak_os_mmap(&ptr_mm_cma->mm, mm_info, vma);
    adlak_cma_release_dev();
    return ret;
}

static void adlak_cma_unmap_userspace(struct adlak_mem_handle *mm_info) {
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    adlak_os_unmmap_userspace(&ptr_mm_cma->mm, mm_info);
    adlak_cma_release_dev();
}

static int adlak_cma_mmap_userspace(struct adlak_mem_handle *mm_info) {
    int ret;
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    ret = adlak_os_mmap2userspace(&ptr_mm_cma->mm, mm_info);
    adlak_cma_release_dev();
    return ret;
}

static void adlak_cma_vunmap(struct adlak_mem_handle *mm_info) {
    AML_LOG_DEBUG("%s", __func__);
    adlak_os_mm_vunmap(mm_info);
}

static void *adlak_cma_vmap(struct adlak_mem_handle *mm_info) {
    AML_LOG_DEBUG("%s", __func__);
    return adlak_os_mm_vmap(mm_info);
}

static void adlak_cma_get_usage(struct adlak_mem_usage *usage) {
    AML_LOG_DEBUG("%s", __func__);
    adlak_cma_claim_dev();
    ptr_mm_cma->usage.share_buf_size = ptr_mm_cma->share_buf.share_buf_size;
    adlak_os_memcpy(usage, &ptr_mm_cma->usage, sizeof(*usage));
    adlak_cma_release_dev();
}

static void adlak_mem_cma_deinit(void) {
#if CONFIG_ADLAK_MEM_POOL_EN
    if (ptr_mm_cma->has_mem_pool) {
        adlak_mem_pools_mgr_deinit();
        ptr_mm_cma->usage.pool_size = 0;
    }

#if defined(CONFIG_ADLAK_USE_RESERVED_MEMORY)
    // destroy unCacheable memory pool from reserved memory
    adlak_unmap_region_nocache(&ptr_mm_cma->mem_pool);
#else

#if defined(CONFIG_ADLA_FREERTOS)
#error "Not support CMA in freertos.Please change with reserved_memory or others"
#endif

    // destroy unCacheable memory pool from CMA
    adlak_free_cma_region_nocache();
#endif
#endif

#ifndef CONFIG_ADLA_FREERTOS
    adlak_cma_deinit(ptr_mm_cma->dev);
#endif

    adlak_os_mutex_destroy(&ptr_mm_cma->mutex);
}

static int adlak_mem_cma_init(void) {
    int                      ret          = 0;
    struct adlak_mem_exclude exclude_area = {0};

    adlak_os_mutex_init(&ptr_mm_cma->mutex);
#if CONFIG_ADLAK_MEM_POOL_EN
#ifndef CONFIG_ADLA_FREERTOS
    ret = adlak_cma_init(ptr_mm_cma->dev);
    if (ret) {
        ret = -1;
        goto err;
    }
#endif

#if defined(CONFIG_ADLAK_USE_RESERVED_MEMORY)
    // Create unCacheable memory pool from reserved memory
    if (0 != adlak_remap_region_nocache(ptr_mm_cma->padlak, &ptr_mm_cma->mem_pool)) {
        ret = -1;
        goto err;
    }
#else
#if defined(CONFIG_ADLA_FREERTOS)
#error "Not support CMA in freertos.Please change with reserved_memory or others"
#endif
    // Create unCacheable memory pool from CMA
    if (0 != adlak_alloc_cma_region_nocache()) {
        ret = -1;
        goto err;
    }
#endif

    if (!ret) {
        ptr_mm_cma->has_mem_pool = true;
    }
    if (ptr_mm_cma->has_mem_pool) {
        ptr_mm_cma->usage.pool_size = 0;
        // init bitmap for memory pools
        if (0 != adlak_mem_pools_mgr_init(&exclude_area)) {
            goto err;
        }
    }

#endif
err:
    return ret;
}

void adlak_mem_cma_unregister(struct adlak_device *padlak, struct adlak_mem_operator *ops) {
    AML_LOG_INFO("%s", __func__);
    if (ptr_mm_cma) {
        adlak_mem_cma_deinit();
        adlak_os_free(ptr_mm_cma);
        ptr_mm_cma = NULL;
    }
    ops->alloc         = NULL;
    ops->free          = NULL;
    ops->attach        = NULL;
    ops->dettach       = NULL;
    ops->mmap          = NULL;
    ops->unmap         = NULL;
    ops->vmap          = NULL;
    ops->vunmap        = NULL;
    ops->cache_flush   = NULL;
    ops->cache_invalid = NULL;
    ops->get_usage     = NULL;
}

int adlak_mem_cma_register(struct adlak_device *padlak, struct adlak_mem_operator *ops) {
    int ret = 0;
    adlak_mem_cma_unregister(padlak, ops);
    AML_LOG_INFO("%s", __func__);
    if ((!ptr_mm_cma)) {
        ptr_mm_cma = adlak_os_zalloc(sizeof(struct adlak_mem_cma), ADLAK_GFP_KERNEL);
        if (unlikely(!ptr_mm_cma)) {
            ret = -1;
            goto end;
        }
    }

    ptr_mm_cma->padlak       = padlak;
    ptr_mm_cma->has_mem_pool = 0;

#ifndef CONFIG_ADLA_FREERTOS
    ptr_mm_cma->dev    = padlak->dev;
    ptr_mm_cma->mm.dev = padlak->dev;
#endif
    ptr_mm_cma->usage.alloced_kmd = 0;
    ptr_mm_cma->usage.alloced_umd = 0;
    ptr_mm_cma->usage.pool_size   = -1;

    /*Share swap memory between different models to avoid dram size usage*/
    ptr_mm_cma->share_buf.ref_cnt          = 0;
    ptr_mm_cma->share_buf.offset_start     = -1;
    ptr_mm_cma->share_buf.bitmap_count_max = 0;
    ptr_mm_cma->share_buf.share_swap_en    = padlak->share_swap_en;
    if (ptr_mm_cma->share_buf.share_swap_en) {
        adlak_os_printf("share buffer enabled\n");
        ptr_mm_cma->share_buf.share_buf_size      = padlak->share_buf_size;
        ptr_mm_cma->share_buf.share_buf_cpu_addr  = 0;
        ptr_mm_cma->share_buf.share_buf_dma_addr  = 0;
        ptr_mm_cma->share_buf.share_buf_phys_addr = 0;
    }

    ops->alloc           = adlak_cma_alloc;
    ops->free            = adlak_cma_free;
    ops->attach          = adlak_cma_attach;
    ops->dettach         = adlak_cma_dettach;
    ops->mmap            = adlak_cma_mmap;
    ops->unmap           = adlak_cma_unmap;
    ops->mmap_userspace  = adlak_cma_mmap_userspace;
    ops->unmap_userspace = adlak_cma_unmap_userspace;
    ops->vmap            = adlak_cma_vmap;
    ops->vunmap          = adlak_cma_vunmap;
    ops->cache_flush     = adlak_cma_cache_flush;
    ops->cache_invalid   = adlak_cma_cache_invalid;
    ops->get_usage       = adlak_cma_get_usage;

    adlak_mem_cma_init();

end:
    return ret;
}
