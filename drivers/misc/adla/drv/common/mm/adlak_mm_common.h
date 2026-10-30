/*******************************************************************************
 * Copyright (C) 2022 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm_common.h
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

#ifndef __ADLAK_MM_COMMON_H__
#define __ADLAK_MM_COMMON_H__

/***************************** Include Files *********************************/

#include "adlak_common.h"
#include "adlak_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/************************** Constant Definitions *****************************/

#define ADLAK_INVALID_ADDR (0x1UL)
#define ADLAK_MEM_UID_SIZE (512 * 1024)  // the uid use to manage memory

#ifndef ADLAK_MM_POOL_PAGE_SHIFT
#define ADLAK_MM_POOL_PAGE_SHIFT (12)
#endif
#define ADLAK_MM_POOL_PAGE_SIZE (1UL << ADLAK_MM_POOL_PAGE_SHIFT)
#define ADLAK_MM_POOL_PAGE_MASK (~(ADLAK_MM_POOL_PAGE_SIZE - 1))

/**************************Global Variable************************************/

/**************************Type Definition and Structure**********************/

struct adlak_mm_pool_priv {
    void *           bitmap;
    size_t           bitmap_size;
    dma_addr_t       addr_base;
    size_t           bits;  // bitmap_maxno
    size_t           used_count;
    adlak_os_mutex_t lock;
};

struct adlak_resv_mem {
    uint32_t                  mem_type;
    void *                    cpu_addr_base;
    phys_addr_t               phys_addr_base;
    dma_addr_t                dma_addr_base;
    size_t                    size;
    struct adlak_mm_pool_priv mm_pool;
};

enum adlak_mem_src {
    ADLAK_ENUM_MEMSRC_CMA = 0,
    ADLAK_ENUM_MEMSRC_RESERVED,
    ADLAK_ENUM_MEMSRC_OS,
    ADLAK_ENUM_MEMSRC_MBP,
    ADLAK_ENUM_MEMSRC_EXT_PHYS,
};

enum adlak_mem_type_inner {
    ADLAK_ENUM_MEMTYPE_INNER_USER_CACHEABLE   = (1u << 0),
    ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS       = (1u << 1),
    ADLAK_ENUM_MEMTYPE_INNER_PA_WITHIN_4G     = (1u << 4),  // physical address less than 4Gbytes
    ADLAK_ENUM_MEMTYPE_INNER_SHARE            = (1u << 5),
    ADLAK_ENUM_MEMTYPE_INNER_KERNEL_CACHEABLE = (1u << 6),
    ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_DEF     = (1u << 7),  //
    ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_ID1     = (1u << 8),  //
    ADLAK_ENUM_MEMTYPE_INNER_SMMU_PRIV        = (1u << 9)   //
};

struct adlak_mem_pool_info {
    uint32_t           cacheable;  //
    size_t             size;
    enum adlak_mem_src mem_src;  // RESERVED_MEM,CMA_MEM,MBP_MEM

    void *                    cpu_addr_base;
    phys_addr_t               phys_addr_base;
    dma_addr_t                dma_addr_base;
    struct adlak_mm_pool_priv bitmap_area;
};

struct adlak_mem_exclude {
    size_t start;
    size_t size;
};

/**
 *  statistics of memory usage
 */
struct adlak_mem_usage {
    size_t pool_size;
    size_t alloced_umd;
    size_t alloced_kmd;
    size_t share_buf_size;
};

struct adlak_mem_inner_req_info {
    enum adlak_mem_type_inner mem_type;       // cacheable,contiguous
    enum adlak_mem_direction  mem_direction;  // rd&wr,rd,wr
    size_t                    bytes;          // align with 4096
};

struct adlak_mem_handle {
    struct adlak_mem_inner_req_info req;
    int                             nr_pages;
    void *                          cpu_addr;
    void *                          cpu_addr_user;
    phys_addr_t                     phys_addr;
    dma_addr_t                      iova_addr;
    dma_addr_t                      dma_addr;
    void *                          sgt;
    void *                          pages;
    phys_addr_t *                   phys_addrs;
    enum dma_data_direction         direction;
    enum adlak_mem_src              mem_src;
    uint32_t                        mem_type;  // the mem_type may not same as request info
    uint32_t                        from_pool;
    uint64_t                        uid;
};

/*Share swap memory between different models to avoid dram size usage,
    but sacrifices performance when executing multiple models simultaneously*/
struct adlak_share_swap_buf {
    unsigned int ref_cnt;          /*reference count of share memory*/
    unsigned int offset_start;     /*record the min value of start addr */
    unsigned int bitmap_count_max; /*record the max number of bitmap*/
    unsigned int share_swap_en;
    unsigned int share_buf_size;
    phys_addr_t  share_buf_phys_addr;
    void *       share_buf_cpu_addr;
    dma_addr_t   share_buf_dma_addr;
};

struct adlak_sync_cache_ext_info {
    /*If the value of is_partial is 0, the values of offset and size will be ignored*/
    int          is_partial;
    unsigned int offset;
    size_t       size;
};

struct adlak_mem_operator {
    struct adlak_mem_handle *(*alloc)(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                      struct adlak_buf_req *          pbuf_req);
    void (*free)(struct adlak_context_smmu_attr *ctx_smmu_attr, struct adlak_mem_handle *mm_info);
    /**
     * bind external memory from user space
     * @param ctx_smmu_attr
     * @param pbuf_req
     * @return
     */
    struct adlak_mem_handle *(*attach)(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                       struct adlak_extern_buf_info *  pbuf_req);

    /**
     * unbind external memory from user space
     * @param ctx_smmu_attr
     * @param mm_info
     */
    void (*dettach)(struct adlak_context_smmu_attr *ctx_smmu_attr,
                    struct adlak_mem_handle *       mm_info);

    int (*mmap)(struct adlak_mem_handle *mm_info, void *const vma);
    int (*unmap)(struct adlak_mem_handle *mm_info);
    int (*mmap_userspace)(struct adlak_mem_handle *mm_info);
    void (*unmap_userspace)(struct adlak_mem_handle *mm_info);
    void *(*vmap)(struct adlak_mem_handle *mm_info);
    void (*vunmap)(struct adlak_mem_handle *mm_info);
    int (*cache_flush)(struct adlak_mem_handle *         mm_info,
                       struct adlak_sync_cache_ext_info *sync_cache_ext_info);
    int (*cache_invalid)(struct adlak_mem_handle *         mm_info,
                         struct adlak_sync_cache_ext_info *sync_cache_ext_info);
    void (*smmu_init)(void *, uint8_t);
    void (*smmu_deinit)(void *, uint8_t);
    void (*smmu_tlb_invalidate)(struct adlak_context_smmu_attr *ctx_smmu_attr);
    void (*get_usage)(struct adlak_mem_usage *usage);
    uint64_t (*get_smmu_entry)(struct adlak_context_smmu_attr *ctx_smmu_attr, uint8_t type);
    void (*smmu_tlb_dump)(struct adlak_context_smmu_attr *ctx_smmu_attr);
};

struct adlak_mem {
#ifndef CONFIG_ADLA_FREERTOS
    struct device *dev;
#else
    void *rsv;
#endif
};

struct adlak_mem_obj {
    struct adlak_mem_operator ops;
    uint8_t                   use_smmu;
    uint8_t                   use_mbd;
};

/************************** Function Prototypes ******************************/

int  adlak_bitmap_pool_init(struct adlak_mm_pool_priv *pool, size_t pool_size,
                            struct adlak_mem_exclude *exclude_area);
void adlak_bitmap_pool_deinit(struct adlak_mm_pool_priv *pool);

unsigned long adlak_alloc_from_bitmap_pool(struct adlak_mm_pool_priv *pool, size_t size);
unsigned long adlak_alloc_from_bitmap_pool_reverse(struct adlak_mm_pool_priv *pool, size_t size);

void adlak_free_to_bitmap_pool(struct adlak_mm_pool_priv *pool, dma_addr_t start, size_t size);

void adlak_set_bitmap_pool(struct adlak_mm_pool_priv *pool, dma_addr_t start, size_t size);

void adlak_bitmap_dump(struct adlak_mm_pool_priv *pool);

int adlak_mem_uid_init(void);

void adlak_mem_uid_deinit(void);

uint64_t adlak_mem_uid_alloc(void);

void adlak_mem_uid_free(uint64_t uid);

int  adlak_simple_bitmap_pool_init(struct adlak_simple_bitmap *p_bitmap);
void adlak_simple_bitmap_pool_deinit(struct adlak_simple_bitmap *p_bitmap);
int  adlak_simple_bitmap_alloc(struct adlak_simple_bitmap *p_bitmap);
void adlak_simple_bitmap_free(struct adlak_simple_bitmap *p_bitmap, int id);

#ifdef __cplusplus
}
#endif

#endif /* __ADLAK_MM_COMMON_H__ end define*/
