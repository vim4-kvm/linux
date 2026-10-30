/*******************************************************************************
 * Copyright (C) 2022 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm_smmu.c
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
#include "adlak_mm_smmu.h"

#include "adlak_device.h"
#include "adlak_mm.h"
#include "adlak_mm_common.h"
#include "adlak_mm_os_common.h"

/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

struct adlak_smmu_tlb_info {
    struct adlak_mem_handle *mm_info;
    size_t                   size;
};

struct adlak_smmu_tlb {
    uint32_t                    iova_size_GB;  // unit is Gbyte
    struct adlak_smmu_tlb_info *tlb_info;
    struct adlak_smmu_tlb_info *ptr_tlb_l1;
    struct adlak_smmu_tlb_info *ptr_tlb_l2;
    struct adlak_smmu_tlb_info *ptr_tlb_l3;
};

struct adlak_smmu_object {
    struct list_head          head;
    size_t                    iova_byte;  // the total size of iova
    uint64_t                  smmu_entry;
    struct adlak_mm_pool_priv bitmap_area;
    struct adlak_smmu_tlb     tlb;
};

struct adlak_mem_smmu {
    adlak_os_mutex_t          mutex;
    struct adlak_device *     padlak;
    struct adlak_mem          mm;
    struct adlak_mem_exclude  exclude_area;
    struct adlak_mem_usage    usage;
    struct list_head          smmu_list;
    struct adlak_smmu_object *smmu_public;
};

/***************** Macros (Inline Functions) Definitions *********************/
#ifdef CONFIG_64BIT
#define _write_page_entry(page_entry, entry_value) \
    *(uint64_t *)(page_entry) = (uint64_t)(entry_value)
#define _read_page_entry(page_entry) *(uint64_t *)(page_entry)
#else

#define _write_page_entry(page_entry, entry_value) \
    *(uint32_t *)(page_entry) = (uint32_t)(entry_value)
#define _read_page_entry(page_entry) *(uint32_t *)(page_entry)
#endif

/************************** Variable Definitions *****************************/
static struct adlak_mem_smmu *ptr_mm_smmu = NULL;

/************************** Function Prototypes ******************************/

static void adlak_smmu_free_inner(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                  struct adlak_mem_handle *mm_info, uint8_t has_iova);

static struct adlak_mem_handle *adlak_smmu_alloc_inner(
    struct adlak_context_smmu_attr *ctx_smmu_attr, struct adlak_buf_req *pbuf_req,
    uint8_t has_iova);
static void *adlak_smmu_vmap(struct adlak_mem_handle *mm_info);

static void inline adlak_smmu_claim_dev(void) {
    AML_LOG_DEBUG("%s[+]", __func__);
    adlak_os_mutex_lock(&ptr_mm_smmu->mutex);
    AML_LOG_DEBUG("%s[-]", __func__);
}
static void inline adlak_smmu_release_dev(void) {
    AML_LOG_DEBUG("%s[+]", __func__);
    adlak_os_mutex_unlock(&ptr_mm_smmu->mutex);
    AML_LOG_DEBUG("%s[-]", __func__);
}

static int adlak_smmu_tlb_alloc(struct adlak_smmu_object *ptr_smmu) {
    int                         ret = 0;
    struct adlak_mem_handle *   mm_info;
    int                         idx1, idx2;
    uint32_t                    tlb1_size, tlb_info_count;
    struct adlak_smmu_tlb *     tlb = &ptr_smmu->tlb;
    struct adlak_smmu_tlb_info *tlb_info;

    struct adlak_buf_req buf_req;
#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
#endif
#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_INFO("Alloc tlb1 buffer.");
#endif
    AML_LOG_INFO("IOVA max space is %dG byte.", tlb->iova_size_GB);
    tlb_info_count = 1;                                                // tlb1;
    tlb_info_count += tlb->iova_size_GB;                               // tlb2;
    tlb_info_count += (tlb->iova_size_GB * SMMU_TLB2_ENTRY_COUNT_2M);  // tlb3;

    tlb->tlb_info =
        adlak_os_malloc(tlb_info_count * sizeof(struct adlak_smmu_tlb_info), ADLAK_GFP_KERNEL);
    if (ADLAK_IS_ERR_OR_NULL(tlb->tlb_info)) {
        ret = -1;
        AML_LOG_ERR("alloc tlb info failed.");
        goto err;
    } else {
        tlb->ptr_tlb_l1 = &tlb->tlb_info[0];
        tlb->ptr_tlb_l2 = &tlb->tlb_info[1];
        tlb->ptr_tlb_l3 = &tlb->tlb_info[1 + tlb->iova_size_GB];
    }

#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_INFO("Alloc tlb1 buffer.");
#endif

    tlb1_size = tlb->iova_size_GB * SMMU_TLB_ENTRY_SIZE;

    tlb->ptr_tlb_l1->size = ADLAK_ALIGN(tlb1_size, SMMU_ALIGN_SIZE);

    buf_req.mem_type      = ADLAK_ENUM_MEMTYPE_CONTIGUOUS | ADLAK_ENUM_MEMTYPE_INNER;
    buf_req.mem_direction = ADLAK_ENUM_MEM_DIR_WRITE_ONLY;

    buf_req.bytes = tlb->ptr_tlb_l1->size;
    mm_info       = adlak_smmu_alloc_inner(NULL, &buf_req, 0);
    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        ret = -1;
        AML_LOG_ERR("adlak_os_alloc_contiguous failed.");
        goto err;
    }
    tlb->ptr_tlb_l1->mm_info = mm_info;

#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_INFO("Alloc tlb2 buffer.");
#endif
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        tlb_info         = &tlb->ptr_tlb_l2[idx1];
        tlb_info->size   = ADLAK_ALIGN(SMMU_TLB2_SIZE, SMMU_ALIGN_SIZE);
        buf_req.mem_type = ADLAK_ENUM_MEMTYPE_CONTIGUOUS | ADLAK_ENUM_MEMTYPE_INNER;
        buf_req.bytes    = tlb_info->size;
        mm_info          = adlak_smmu_alloc_inner(NULL, &buf_req, 0);
        if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
            ret = -1;
            AML_LOG_ERR("adlak_os_alloc_contiguous failed.");
            goto err;
        }
        tlb_info->mm_info = mm_info;
    }
#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_INFO("Alloc tlb3 buffer.");
#endif
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M; idx2++) {
            tlb_info         = &tlb->ptr_tlb_l3[idx1 * SMMU_TLB2_ENTRY_COUNT_2M + idx2];
            tlb_info->size   = ADLAK_ALIGN(SMMU_TLB3_SIZE, SMMU_ALIGN_SIZE);
            buf_req.mem_type = ADLAK_ENUM_MEMTYPE_CONTIGUOUS | ADLAK_ENUM_MEMTYPE_INNER;
            buf_req.bytes    = tlb_info->size;
            mm_info          = adlak_smmu_alloc_inner(NULL, &buf_req, 0);
            if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
                ret = -1;
                AML_LOG_ERR("adlak_os_alloc_contiguous failed.");
                goto err;
            }
            tlb_info->mm_info = mm_info;
        }
    }

    AML_LOG_DEBUG("tlb1 section size=%lu Bytes.", (uintptr_t)tlb->ptr_tlb_l1->size);
    AML_LOG_DEBUG("tlb2 per-section size=%lu KBytes.",
                  (uintptr_t)(&tlb->ptr_tlb_l2[0])->size / 1024);
    AML_LOG_DEBUG("tlb3 per-section size=%lu KBytes.",
                  (uintptr_t)(&tlb->ptr_tlb_l3[0 * 0])->size / 1024);

    return 0;

err:

    return ret;
}

static int adlak_smmu_tlb_fill_init(struct adlak_smmu_object *ptr_smmu) {
    int                         idx1, idx2, idx3;
    uintptr_t                   tlb_logic;
    uint64_t                    entry_val = 0;
    struct adlak_smmu_tlb *     tlb       = &ptr_smmu->tlb;
    struct adlak_smmu_tlb_info *tlb_info;

#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
#endif

    /*fill tlb level 1*/
    tlb_logic = (uintptr_t)adlak_smmu_vmap(tlb->ptr_tlb_l1->mm_info);
    AML_LOG_DEBUG("l1 cpu_addr= %lX,mem_type=%u",
                  (uintptr_t)adlak_smmu_vmap(tlb->ptr_tlb_l1->mm_info),
                  tlb->ptr_tlb_l1->mm_info->mem_type);

    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        tlb_info  = &tlb->ptr_tlb_l2[idx1];
        entry_val = tlb_info->mm_info->phys_addr;
        entry_val = entry_val | SMMU_ENTRY_FLAG_L1_VALID;
#ifdef CONFIG_64BIT
        _write_page_entry(tlb_logic + (idx1 * SMMU_TLB_ENTRY_SIZE), entry_val);
#else
        _write_page_entry(tlb_logic + (idx1 * SMMU_TLB_ENTRY_SIZE), (uint32_t)entry_val);
        _write_page_entry(tlb_logic + (idx1 * SMMU_TLB_ENTRY_SIZE) + sizeof(uint32_t),
                          (uint32_t)(entry_val >> 32));
#endif
    }

    /*fill tlb level 2*/
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        tlb_info  = &tlb->ptr_tlb_l2[idx1];
        tlb_logic = (uintptr_t)adlak_smmu_vmap(tlb_info->mm_info);
        for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M; idx2++) {
            tlb_info  = &tlb->ptr_tlb_l3[idx1 * SMMU_TLB2_ENTRY_COUNT_2M + idx2];
            entry_val = tlb_info->mm_info->phys_addr;
            entry_val = entry_val | SMMU_ENTRY_FLAG_L2_VALID_4K;
#ifdef CONFIG_64BIT
            _write_page_entry(tlb_logic + (idx2 * SMMU_TLB_ENTRY_SIZE), entry_val);
#else
            _write_page_entry(tlb_logic + (idx2 * SMMU_TLB_ENTRY_SIZE), (uint32_t)entry_val);
            _write_page_entry(tlb_logic + (idx2 * SMMU_TLB_ENTRY_SIZE) + sizeof(uint32_t),
                              (uint32_t)(entry_val >> 32));
#endif
        }
    }

    /*fill tlb level 3*/
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M; idx2++) {
            tlb_info  = &tlb->ptr_tlb_l3[idx1 * SMMU_TLB2_ENTRY_COUNT_2M + idx2];
            tlb_logic = (uint64_t)(uintptr_t)adlak_smmu_vmap(tlb_info->mm_info);
            for (idx3 = 0; idx3 < SMMU_TLB3_ENTRY_COUNT_4K; idx3++) {
                entry_val = 0;
                entry_val = entry_val | SMMU_ENTRY_FLAG_L3_INVALID;
#ifdef CONFIG_64BIT
                _write_page_entry(tlb_logic + (idx3 * SMMU_TLB_ENTRY_SIZE), entry_val);
#else
                _write_page_entry(tlb_logic + (idx3 * SMMU_TLB_ENTRY_SIZE), entry_val);
                _write_page_entry(tlb_logic + (idx3 * SMMU_TLB_ENTRY_SIZE) + sizeof(uint32_t),
                                  (uint32_t)(entry_val >> 32));
#endif
            }
        }
    }

    ptr_smmu->smmu_entry = tlb->ptr_tlb_l1->mm_info->phys_addr;

    return 0;
}

#ifdef CONFIG_ADLAK_DEBUG_SMMU_TLB_DUMP
static int adlak_smmu_tlb_dump_inner(struct adlak_smmu_object *ptr_smmu) {
    struct adlak_smmu_tlb *tlb = NULL;
    int                    idx1, idx2, idx3;
    uintptr_t              tlb_logic;

    if (NULL == ptr_smmu) {
        return -1;
    }

    adlak_os_printf("%s", __func__);
    tlb = &ptr_smmu->tlb;
    adlak_os_printf("TLB level 1");
    adlak_os_printf("phys_addr: 0x%08X\n", (uintptr_t)tlb->ptr_tlb_l1->mm_info->phys_addr);
    tlb_logic = (uintptr_t)adlak_smmu_vmap(tlb->ptr_tlb_l1->mm_info);
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
#ifdef CONFIG_64BIT
        adlak_os_printf("offset:0x%08X\t0x%llX \n", (uint32_t)(idx1 * sizeof(uint64_t)),
                        _read_page_entry(tlb_logic + (idx1 * sizeof(uint64_t))));
#else
        adlak_os_printf("offset:0x%08X\t0x%08X 0x%08X\n", (uint32_t)(idx1 * sizeof(uint64_t)),
                        _read_page_entry(tlb_logic + (idx1 * sizeof(uint32_t))),
                        _read_page_entry(tlb_logic + ((idx1 + 1) * sizeof(uint32_t))));

#endif
    }
    adlak_os_printf("TLB level 2");
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        adlak_os_msleep(50);
        adlak_os_printf("TLB level 2 idx=%d", idx1);
        adlak_os_printf("phys_addr: 0x%08X\n", (uintptr_t)tlb->ptr_tlb_l2[idx1].mm_info->phys_addr);
        tlb_logic = (uintptr_t)adlak_smmu_vmap(tlb->ptr_tlb_l2[idx1].mm_info);
        for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M;) {
#ifdef CONFIG_64BIT
            adlak_os_printf("offset:0x%08X\t0x%llX 0x%llX 0x%llX 0x%llX \n",
                            (uint32_t)(idx2 * sizeof(uint64_t)),
                            _read_page_entry(tlb_logic + (idx2 * sizeof(uint64_t))),
                            _read_page_entry(tlb_logic + ((idx2 + 1) * sizeof(uint64_t))),
                            _read_page_entry(tlb_logic + ((idx2 + 2) * sizeof(uint64_t))),
                            _read_page_entry(tlb_logic + ((idx2 + 3) * sizeof(uint64_t))));
#else
            adlak_os_printf(
                "offset:0x%08X\t0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
                (uint32_t)(idx2 * sizeof(uint64_t)),
                _read_page_entry(tlb_logic + (idx2 * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 1) * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 2) * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 3) * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 4) * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 5) * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 6) * sizeof(uint32_t))),
                _read_page_entry(tlb_logic + ((idx2 + 7) * sizeof(uint32_t))));

#endif
            idx2 = idx2 + 4;
        }

        adlak_os_printf("\n");
    }

    adlak_os_printf("TLB level 3");

    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        adlak_os_msleep(50);
        for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M; idx2++) {
            adlak_os_msleep(50);
            adlak_os_printf("TLB level 3 [%d][%d]", idx1, idx2);
            adlak_os_printf("phys_addr: 0x%08X\n",
                            (uintptr_t)tlb->ptr_tlb_l3[idx1 * SMMU_TLB2_ENTRY_COUNT_2M + idx2]
                                .mm_info->phys_addr);
            tlb_logic = (uintptr_t)adlak_smmu_vmap(
                tlb->ptr_tlb_l3[idx1 * SMMU_TLB2_ENTRY_COUNT_2M + idx2].mm_info);
            for (idx3 = 0; idx3 < SMMU_TLB2_ENTRY_COUNT_2M;) {
#ifdef CONFIG_64BIT
                adlak_os_printf("offset:0x%08X\t0x%llX 0x%llX 0x%llX 0x%llX \n",
                                (uint32_t)(idx3 * sizeof(uint64_t)),
                                _read_page_entry(tlb_logic + (idx3 * sizeof(uint64_t))),
                                _read_page_entry(tlb_logic + ((idx3 + 1) * sizeof(uint64_t))),
                                _read_page_entry(tlb_logic + ((idx3 + 2) * sizeof(uint64_t))),
                                _read_page_entry(tlb_logic + ((idx3 + 3) * sizeof(uint64_t))));
#else
                adlak_os_printf(
                    "offset:0x%08X\t0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n",
                    (uint32_t)(idx3 * sizeof(uint64_t)),
                    _read_page_entry(tlb_logic + (idx3 * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 1) * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 2) * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 3) * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 4) * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 5) * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 6) * sizeof(uint32_t))),
                    _read_page_entry(tlb_logic + ((idx3 + 7) * sizeof(uint32_t))));

#endif
                idx3 = idx3 + 4;
            }
        }
    }
    return 0;
}
#endif

static void adlak_smmu_tlb_dump(struct adlak_context_smmu_attr *ctx_smmu_attr) {
#ifdef CONFIG_ADLAK_DEBUG_SMMU_TLB_DUMP
    struct adlak_smmu_object *smmu_public  = NULL;
    struct adlak_smmu_object *ptr_smmu_pri = NULL;

    adlak_os_printf("%s", __func__);
    if (ctx_smmu_attr) {
        adlak_smmu_claim_dev();
        smmu_public = ctx_smmu_attr->smmu_public;
        adlak_smmu_tlb_dump_inner(smmu_public);
        ptr_smmu_pri = ctx_smmu_attr->smmu_private;
        adlak_smmu_tlb_dump_inner(ptr_smmu_pri);
        adlak_smmu_release_dev();
    }
#endif
}

static int adlak_smmu_tlb_add(struct adlak_smmu_object *ptr_smmu, dma_addr_t iova_addr,
                              phys_addr_t phys_addr, size_t size) {
    struct adlak_smmu_tlb *tlb = &ptr_smmu->tlb;
    uint32_t               tlb_l1_offset, tlb_l2_offset, tlb_l3_offset;
    uintptr_t              tlb_logic;
    uint64_t               entry_val = 0;

#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
#endif
#if ADLAK_DEBUG_SMMU_EN

    AML_LOG_DEBUG("iova_addr = 0x%llx, phys_addr = 0x%llx ", (uint64_t)iova_addr,
                  (uint64_t)phys_addr);
#endif
    tlb_l1_offset = GET_SMMU_TLB1_ENTRY_OFFSEST(iova_addr);
    tlb_l2_offset = GET_SMMU_TLB2_ENTRY_OFFSEST(iova_addr);
    tlb_l3_offset = GET_SMMU_TLB3_ENTRY_OFFSEST(iova_addr);
#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("tlb_l1_offset = %d, tlb_l2_offset = %d, tlb_l3_offset = %d ", tlb_l1_offset,
                  tlb_l2_offset, tlb_l3_offset);
#endif

    tlb_logic = (uintptr_t)(adlak_smmu_vmap(
        tlb->ptr_tlb_l3[tlb_l1_offset * SMMU_TLB2_ENTRY_COUNT_2M + tlb_l2_offset].mm_info));
    entry_val = phys_addr;
    entry_val = entry_val | SMMU_ENTRY_FLAG_L3_VALID;
#if ADLAK_DEBUG_SMMU_EN
#ifdef CONFIG_64BIT
    AML_LOG_DEBUG("tlb_logic = 0x%llx+0x%x, entry_val = 0x%llx ", (uint64_t)tlb_logic,
                  (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE), (uint64_t)entry_val);
#else
    AML_LOG_DEBUG("tlb_logic = 0x%llx+0x%x, entry_val = 0x%llx ",
                  (uint64_t)(tlb_logic & 0xFFFFFFFF), (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE),
                  (uint64_t)entry_val);
#endif
#endif

#ifdef CONFIG_64BIT
    _write_page_entry(tlb_logic + (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE), entry_val);
#else
    _write_page_entry(tlb_logic + (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE), (uint32_t)entry_val);
    _write_page_entry(tlb_logic + (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE) + sizeof(uint32_t),
                      (uint32_t)(entry_val >> 32));
#endif
    return 0;
}

static int adlak_smmu_tlb_del(struct adlak_smmu_object *ptr_smmu, dma_addr_t iova_addr) {
    struct adlak_smmu_tlb *tlb = &ptr_smmu->tlb;
    uint32_t               tlb_l1_offset, tlb_l2_offset, tlb_l3_offset;
    uintptr_t              tlb_logic;
    uint64_t               entry_val = 0;

#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
    AML_LOG_DEBUG("tlb_del iova_addr = 0x%llx ", (uint64_t)iova_addr);
#endif
    tlb_l1_offset = GET_SMMU_TLB1_ENTRY_OFFSEST(iova_addr);
    tlb_l2_offset = GET_SMMU_TLB2_ENTRY_OFFSEST(iova_addr);
    tlb_l3_offset = GET_SMMU_TLB3_ENTRY_OFFSEST(iova_addr);
    tlb_logic     = (uintptr_t)adlak_smmu_vmap(
        tlb->ptr_tlb_l3[tlb_l1_offset * SMMU_TLB2_ENTRY_COUNT_2M + tlb_l2_offset].mm_info);
    entry_val = 0;
    entry_val = entry_val | SMMU_ENTRY_FLAG_L3_INVALID;
#ifdef CONFIG_64BIT
    _write_page_entry(tlb_logic + (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE), entry_val);
#else
    _write_page_entry(tlb_logic + (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE), (uint32_t)entry_val);
    _write_page_entry(tlb_logic + (tlb_l3_offset * SMMU_TLB_ENTRY_SIZE) + sizeof(uint32_t),
                      (uint32_t)(entry_val >> 32));
#endif
    return 0;
}

static int adlak_smmu_tlb_deinit(struct adlak_smmu_object *ptr_smmu) {
    int                    idx1, idx2;
    struct adlak_smmu_tlb *tlb = NULL;
    AML_LOG_DEBUG("%s", __func__);

    tlb = &ptr_smmu->tlb;

    if (tlb->tlb_info) {
        if (tlb->ptr_tlb_l1->mm_info) {
            adlak_smmu_free_inner(NULL, tlb->ptr_tlb_l1->mm_info, 0);
        }

        for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
            if (tlb->ptr_tlb_l2[idx1].mm_info) {
                adlak_smmu_free_inner(NULL, tlb->ptr_tlb_l2[idx1].mm_info, 0);
            }
        }
        for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
            for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M; idx2++) {
                if (tlb->ptr_tlb_l3[idx1 * SMMU_TLB2_ENTRY_COUNT_2M + idx2].mm_info) {
                    adlak_smmu_free_inner(
                        NULL, tlb->ptr_tlb_l3[(idx1 * SMMU_TLB2_ENTRY_COUNT_2M) + idx2].mm_info, 0);
                }
            }
        }

        adlak_os_free(tlb->tlb_info);
        tlb->tlb_info = NULL;
    }
    return 0;
}

static int adlak_smmu_tlb_init(struct adlak_smmu_object *ptr_smmu, uint32_t iova_max_size_GB) {
    AML_LOG_DEBUG("%s", __func__);

    if (adlak_smmu_tlb_alloc(ptr_smmu)) {
        adlak_smmu_tlb_deinit(ptr_smmu);
    }
    adlak_smmu_tlb_fill_init(ptr_smmu);
    return 0;
}

static int adlak_smmu_tlb_flush_cache(struct adlak_smmu_object *ptr_smmu) {
    struct adlak_device *            padlak = ptr_mm_smmu->padlak;
    struct adlak_smmu_tlb *          tlb    = &ptr_smmu->tlb;
    uint32_t                         idx1, idx2;
    struct adlak_sync_cache_ext_info sync_cache_extern;

    AML_LOG_DEBUG("%s", __func__);

    /*flush cache level 1*/
    sync_cache_extern.is_partial = 0;
    adlak_flush_cache(padlak, tlb->ptr_tlb_l1->mm_info, &sync_cache_extern);

    /*flush cache level 2*/
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        adlak_flush_cache(padlak, tlb->ptr_tlb_l2[idx1].mm_info, &sync_cache_extern);
    }

    /*flush cache level 3*/
    for (idx1 = 0; idx1 < tlb->iova_size_GB; idx1++) {
        for (idx2 = 0; idx2 < SMMU_TLB2_ENTRY_COUNT_2M; idx2++) {
            adlak_flush_cache(padlak,
                              tlb->ptr_tlb_l3[(idx1 * SMMU_TLB2_ENTRY_COUNT_2M) + idx2].mm_info,
                              &sync_cache_extern);
        }
    }

    return 0;
}

static void adlak_smmu_unmap_iova_pages(struct adlak_smmu_object *smmu,
                                        struct adlak_mem_handle * mm_info) {
    int           smmu_nr_pages;
    int           i;
    unsigned long iova_addr;
    iova_addr = mm_info->iova_addr;
#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
#endif
    smmu_nr_pages = mm_info->nr_pages * (ADLAK_PAGE_SIZE / SMMU_PAGESIZE);

    for (i = 0; i < smmu_nr_pages; ++i) {
        adlak_smmu_tlb_del(smmu, iova_addr);
        if (mm_info->req.bytes < (1 << SMMU_TLB2_VA_SHIFT)) {
            //   adlak_hal_smmu_cache_invalid((void *)mm->padlak, iova_addr);
        }
        iova_addr += SMMU_PAGESIZE;
    }
    if (mm_info->req.bytes >= (1 << SMMU_TLB2_VA_SHIFT)) {
        //  adlak_hal_smmu_cache_invalid((void *)mm->padlak, 0);
    }

    adlak_free_to_bitmap_pool(&smmu->bitmap_area, mm_info->iova_addr, (size_t)mm_info->req.bytes);
}

static void adlak_smmu_iova_unmap(struct adlak_smmu_object *smmu,
                                  struct adlak_mem_handle * mm_info) {
#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
#endif

    if (ADLAK_INVALID_ADDR != mm_info->iova_addr) {
        adlak_smmu_unmap_iova_pages(smmu, mm_info);
    }
}

static int adlak_smmu_iova_map(struct adlak_smmu_object *smmu, struct adlak_mem_handle *mm_info) {
    phys_addr_t phys_addr;
    dma_addr_t  iova_addr;
    int         i, j;

#if ADLAK_DEBUG_SMMU_EN
    AML_LOG_DEBUG("%s", __func__);
#endif
    if (ADLAK_INVALID_ADDR == mm_info->iova_addr) {
        if (!(mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_PRIV)) {
            mm_info->iova_addr =
                adlak_alloc_from_bitmap_pool_reverse(&smmu->bitmap_area, mm_info->req.bytes);
        } else {
            mm_info->iova_addr =
                adlak_alloc_from_bitmap_pool(&smmu->bitmap_area, mm_info->req.bytes);
        }

        if (ADLAK_INVALID_ADDR == mm_info->iova_addr) {
            AML_LOG_ERR("failed to alloc iova!");
            goto early_exit;
        }
    } else {
        adlak_set_bitmap_pool(&smmu->bitmap_area, mm_info->iova_addr, mm_info->req.bytes);
    }

    iova_addr = mm_info->iova_addr;

    AML_LOG_DEBUG("iova_addr = 0x%llx, phys_addr[0] = 0x%llx ", (uint64_t)mm_info->iova_addr,
                  (uint64_t)mm_info->phys_addrs[0]);

    for (i = 0; i < mm_info->nr_pages; ++i) {
        for (j = 0; j < ADLAK_PAGE_SIZE / SMMU_PAGESIZE; ++j) {
            phys_addr = mm_info->phys_addrs[i] + j * SMMU_PAGESIZE;

            if (adlak_smmu_tlb_add(smmu, iova_addr, phys_addr, SMMU_PAGESIZE)) {
                AML_LOG_ERR("failed to map iova 0x%lX pa 0x%lX size %lu\n", (uintptr_t)iova_addr,
                            (uintptr_t)phys_addr, ADLAK_PAGE_SIZE);
                goto unmap_pages;
            }
            if (mm_info->req.bytes < (1 << SMMU_TLB2_VA_SHIFT)) {
                //   adlak_hal_smmu_cache_invalid((void *)mm->padlak, iova_addr);
            }
            iova_addr += SMMU_PAGESIZE;
        }
    }
    if (mm_info->req.bytes >= (1 << SMMU_TLB2_VA_SHIFT)) {
        //  adlak_hal_smmu_cache_invalid((void *)mm->padlak, 0);
    }

    return 0;

unmap_pages:
    adlak_smmu_unmap_iova_pages(smmu, mm_info);

early_exit:
    return ERR(ENOMEM);
}

static int adlak_smmu_cache_flush(struct adlak_mem_handle *         mm_info,
                                  struct adlak_sync_cache_ext_info *sync_cache_ext_info) {
    AML_LOG_DEBUG("%s", __func__);
    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        return -1;
    }
    if (mm_info->mem_src == ADLAK_ENUM_MEMSRC_OS || mm_info->mem_src == ADLAK_ENUM_MEMSRC_CMA ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_RESERVED ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_EXT_PHYS) {
        adlak_os_flush_cache(&ptr_mm_smmu->mm, mm_info, sync_cache_ext_info);
    }
    return 0;
}

static int adlak_smmu_cache_invalid(struct adlak_mem_handle *         mm_info,
                                    struct adlak_sync_cache_ext_info *sync_cache_ext_info) {
    AML_LOG_DEBUG("%s", __func__);
    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        return -1;
    }
    if (mm_info->mem_src == ADLAK_ENUM_MEMSRC_OS || mm_info->mem_src == ADLAK_ENUM_MEMSRC_CMA ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_RESERVED ||
        mm_info->mem_src == ADLAK_ENUM_MEMSRC_EXT_PHYS) {
        adlak_os_invalid_cache(&ptr_mm_smmu->mm, mm_info, sync_cache_ext_info);
    }
    return 0;
}

static void adlak_smmu_rewrite_memtype(struct adlak_buf_req *   pbuf_req,
                                       struct adlak_mem_handle *mm_info) {
    mm_info->req.mem_type = 0;

    if (!(pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_INNER)) {
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS;
    } else {
        if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_CONTIGUOUS) {
            mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS;
        }
    }

    if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_PA_WITHIN_4G) {
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_PA_WITHIN_4G;
    }
    if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_SMMU_PRIV) {
        mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_SMMU_PRIV;
        if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_SMMU_TLB_ID1) {
            mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_ID1;
        }
        if (pbuf_req->mem_type & ADLAK_ENUM_MEMTYPE_SMMU_TLB_DEF) {
            mm_info->req.mem_type = mm_info->req.mem_type | ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_DEF;
        }
    }

    mm_info->mem_type = mm_info->req.mem_type;
}

static int adlak_smmu_iova_alloc(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                 struct adlak_mem_handle *       mm_info) {
    int                       ret          = 0;
    struct adlak_smmu_object *smmu_public  = NULL;
    struct adlak_smmu_object *ptr_smmu_pri = NULL;
    mm_info->iova_addr                     = ADLAK_INVALID_ADDR;

    AML_LOG_DEBUG("%s", __func__);
    if (ctx_smmu_attr == NULL) {
        smmu_public = ptr_mm_smmu->smmu_public;
        ret         = adlak_smmu_iova_map(smmu_public, mm_info);
    } else {
        ctx_smmu_attr->smmu_tlb_updated = 1;
        ptr_smmu_pri                    = ctx_smmu_attr->smmu_private;
        smmu_public                     = ctx_smmu_attr->smmu_public;

        if (ctx_smmu_attr->smmu_tlb_type == ADLAK_ENUM_SMMU_TLB_TYPE_PRIVATE_ONLY) {
            AML_LOG_INFO("iova alloc only in private");
            ret = adlak_smmu_iova_map(ptr_smmu_pri, mm_info);
        } else if (ctx_smmu_attr->smmu_tlb_type == ADLAK_ENUM_SMMU_TLB_TYPE_PRIVATE_AND_PUBLIC) {
            if (!(mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_PRIV)) {
                // smmu_public & smmu_priv have the same iova range
                AML_LOG_INFO("iova alloc both in public and private");
                ret = adlak_smmu_iova_map(smmu_public,
                                          mm_info);  // smmu_public must be map first
                if (!ret) {
                    ret = adlak_smmu_iova_map(ptr_smmu_pri, mm_info);
                }

            } else {
                if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_ID1) {
                    AML_LOG_INFO("iova alloc in private");
                    ret = adlak_smmu_iova_map(ptr_smmu_pri, mm_info);
                }
                if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_DEF) {
                    AML_LOG_INFO("iova alloc in public");
                    ret = adlak_smmu_iova_map(smmu_public, mm_info);
                }
            }
        } else {
            AML_LOG_INFO("iova alloc only in public");
            ret = adlak_smmu_iova_map(smmu_public, mm_info);
        }
    }
    return ret;
}

static void adlak_smmu_iova_free(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                 struct adlak_mem_handle *       mm_info) {
    struct adlak_smmu_object *smmu_public  = NULL;
    struct adlak_smmu_object *ptr_smmu_pri = NULL;

    AML_LOG_DEBUG("%s", __func__);
    if (ctx_smmu_attr == NULL) {
        smmu_public = ptr_mm_smmu->smmu_public;
        adlak_smmu_iova_unmap(smmu_public, mm_info);
    } else {
        ptr_smmu_pri = ctx_smmu_attr->smmu_private;
        smmu_public  = ctx_smmu_attr->smmu_public;

        if (ctx_smmu_attr->smmu_tlb_type == ADLAK_ENUM_SMMU_TLB_TYPE_PRIVATE_ONLY) {
            adlak_smmu_iova_unmap(ptr_smmu_pri, mm_info);
        } else if (ctx_smmu_attr->smmu_tlb_type == ADLAK_ENUM_SMMU_TLB_TYPE_PRIVATE_AND_PUBLIC) {
            if (!(mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_PRIV)) {
                // smmu_public & smmu_priv have the same iova range
                adlak_smmu_iova_unmap(ptr_smmu_pri, mm_info);
                adlak_smmu_iova_unmap(smmu_public, mm_info);

            } else {
                if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_ID1) {
                    adlak_smmu_iova_unmap(ptr_smmu_pri, mm_info);
                }
                if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SMMU_TLB_DEF) {
                    adlak_smmu_iova_unmap(smmu_public, mm_info);
                }
            }
        } else {
            //(ctx_smmu_attr->smmu_tlb_type == ADLAK_ENUM_SMMU_TLB_TYPE_PUBLIC_ONLY)
            adlak_smmu_iova_unmap(smmu_public, mm_info);
        }
    }
    mm_info->iova_addr = ADLAK_INVALID_ADDR;
}

static void adlak_smmu_free_inner(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                  struct adlak_mem_handle *mm_info, uint8_t has_iova) {
    ASSERT(mm_info);

    AML_LOG_DEBUG("%s", __func__);
    if (has_iova) {
        adlak_smmu_iova_free(ctx_smmu_attr, mm_info);
    }

    if (NULL == ctx_smmu_attr) {
        ptr_mm_smmu->usage.alloced_kmd -= mm_info->req.bytes;
    } else {
        ptr_mm_smmu->usage.alloced_umd -= mm_info->req.bytes;
        ctx_smmu_attr->alloc_byte -= mm_info->req.bytes;
    }

    if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS) {
        adlak_os_free_contiguous(&ptr_mm_smmu->mm, mm_info);
    } else {
        adlak_os_free_discontiguous(&ptr_mm_smmu->mm, mm_info);
    }

    adlak_mem_uid_free(mm_info->uid);
    adlak_os_free(mm_info);
}

static struct adlak_mem_handle *adlak_smmu_alloc_inner(
    struct adlak_context_smmu_attr *ctx_smmu_attr, struct adlak_buf_req *pbuf_req,
    uint8_t has_iova) {
    int                      ret = -1;
    struct adlak_mem_handle *mm_info;
    if (!pbuf_req->bytes) {
        return (void *)NULL;
    }
    AML_LOG_DEBUG("%s", __func__);

    mm_info = adlak_os_zalloc(sizeof(struct adlak_mem_handle), ADLAK_GFP_KERNEL);
    if (unlikely(!mm_info)) {
        goto end;
    }

    mm_info->uid               = adlak_mem_uid_alloc();
    mm_info->iova_addr         = ADLAK_INVALID_ADDR;  // init as invalid value
    mm_info->req.bytes         = ADLAK_PAGE_ALIGN(pbuf_req->bytes);
    mm_info->req.mem_direction = pbuf_req->mem_direction;
    adlak_smmu_rewrite_memtype(pbuf_req, mm_info);

    mm_info->nr_pages = ADLAK_DIV_ROUND_UP(mm_info->req.bytes, ADLAK_PAGE_SIZE);
    mm_info->cpu_addr = NULL;
    if (has_iova) {
        AML_LOG_INFO("%s (has_iova) 0x%lX byte", __func__, (uintptr_t)mm_info->req.bytes);
    }

    mm_info->mem_src = ADLAK_ENUM_MEMSRC_OS;
    if (mm_info->req.mem_type & ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS) {
        // alloc contiguous memory
        ret = adlak_os_alloc_contiguous(&ptr_mm_smmu->mm, mm_info);
    } else {
        // alloc discontiguous memory
        ret = adlak_os_alloc_discontiguous(&ptr_mm_smmu->mm, mm_info);
    }
    if (ret) {
        goto err_alloc;
    }

    // update the tlb of smmu
    if (has_iova) {
        ret = adlak_smmu_iova_alloc(ctx_smmu_attr, mm_info);
        if (ret) {
            goto err_iova;
        }
    }
    if (NULL == ctx_smmu_attr) {
        ptr_mm_smmu->usage.alloced_kmd += mm_info->req.bytes;
    } else {
        ptr_mm_smmu->usage.alloced_umd += mm_info->req.bytes;
        ctx_smmu_attr->alloc_byte += mm_info->req.bytes;
    }
end:
    if (ret) {
        AML_LOG_ERR("%s fail!", __FUNCTION__);
    }

    return mm_info;
err_iova:
    if (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_CONTIGUOUS) {
        adlak_os_free_contiguous(&ptr_mm_smmu->mm, mm_info);
    } else {
        adlak_os_free_discontiguous(&ptr_mm_smmu->mm, mm_info);
    }
err_alloc:
    adlak_mem_uid_free(mm_info->uid);
    adlak_os_free(mm_info);
    return NULL;
}

static void adlak_smmu_free(struct adlak_context_smmu_attr *ctx_smmu_attr,
                            struct adlak_mem_handle *       mm_info) {
    adlak_smmu_claim_dev();
    adlak_smmu_free_inner(ctx_smmu_attr, mm_info, 1);
    adlak_smmu_release_dev();
}

static struct adlak_mem_handle *adlak_smmu_alloc(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                                 struct adlak_buf_req *          pbuf_req) {
    struct adlak_mem_handle *ret;
    adlak_smmu_claim_dev();
    ret = adlak_smmu_alloc_inner(ctx_smmu_attr, pbuf_req, 1);
    adlak_smmu_release_dev();
    return ret;
}

static void adlak_smmu_dettach(struct adlak_context_smmu_attr *ctx_smmu_attr,
                               struct adlak_mem_handle *       mm_info) {
    adlak_smmu_claim_dev();
    adlak_smmu_iova_free(ctx_smmu_attr, mm_info);

    adlak_os_dettach_ext_mem(&ptr_mm_smmu->mm, mm_info);
    adlak_mem_uid_free(mm_info->uid);
    adlak_os_free(mm_info);
    adlak_smmu_release_dev();
}

static struct adlak_mem_handle *adlak_smmu_attach(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                                  struct adlak_extern_buf_info *  pbuf_req) {
    int                      ret       = -1;
    struct adlak_mem_handle *mm_info   = NULL;
    uint64_t                 phys_addr = pbuf_req->buf_handle;
    size_t                   size      = pbuf_req->bytes;
    if (!size || !ADLAK_IS_ALIGNED((unsigned long)(size), ADLAK_PAGE_SIZE)) {
        goto early_exit;
    }
    adlak_smmu_claim_dev();

    AML_LOG_DEBUG("%s", __func__);
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
    ret               = adlak_os_attach_ext_mem(&ptr_mm_smmu->mm, mm_info, phys_addr);

    if (!ret) {
        pbuf_req->errcode = 0;
    } else {
        pbuf_req->errcode = ret;
        adlak_mem_uid_free(mm_info->uid);
        adlak_os_free(mm_info);
        mm_info = (void *)NULL;
        goto end;
    }

    // update the tlb of smmu
    ret = adlak_smmu_iova_alloc(ctx_smmu_attr, mm_info);

end:
early_exit:
    if (ret) {
        AML_LOG_ERR("%s fail!", __FUNCTION__);
    }
    adlak_smmu_release_dev();

    return mm_info;
}

static int adlak_smmu_unmap(struct adlak_mem_handle *mm_info) {
    // nothing todo
    return 0;
}

static int adlak_smmu_mmap(struct adlak_mem_handle *mm_info, void *const vma) {
    int ret;
    adlak_smmu_claim_dev();

    ret = adlak_os_mmap(&ptr_mm_smmu->mm, mm_info, vma);
    adlak_smmu_release_dev();
    return ret;
}

static void adlak_smmu_unmap_userspace(struct adlak_mem_handle *mm_info) {
    adlak_smmu_claim_dev();
    adlak_os_unmmap_userspace(&ptr_mm_smmu->mm, mm_info);
    adlak_smmu_release_dev();
}

static int adlak_smmu_mmap_userspace(struct adlak_mem_handle *mm_info) {
    int ret;
    adlak_smmu_claim_dev();

    ret = adlak_os_mmap2userspace(&ptr_mm_smmu->mm, mm_info);
    adlak_smmu_release_dev();

    return ret;
}

static void adlak_smmu_vunmap(struct adlak_mem_handle *mm_info) { adlak_os_mm_vunmap(mm_info); }

static void *adlak_smmu_vmap(struct adlak_mem_handle *mm_info) { return adlak_os_mm_vmap(mm_info); }

static int adlak_smmu_create_obj(struct adlak_smmu_object **pptr_smmu, uint32_t iova_max_size_GB) {
    int                       ret      = 0;
    struct adlak_smmu_object *ptr_smmu = NULL;

    ptr_smmu = adlak_os_zalloc(sizeof(struct adlak_smmu_object), ADLAK_GFP_KERNEL);
    if (unlikely(!ptr_smmu)) {
        ret = -1;
        goto end;
    }

    ptr_smmu->iova_byte = ((size_t)iova_max_size_GB) << 30;

    if (3 < iova_max_size_GB && 0 == ptr_smmu->iova_byte) {
        // Data overflow may occur in 32bit systems
        ptr_smmu->iova_byte = 0xFFFFFFFF - ((sizeof(long) * 8 << ADLAK_MM_POOL_PAGE_SHIFT) - 1);
    }

    ptr_smmu->tlb.iova_size_GB = iova_max_size_GB;
    ret                        = adlak_bitmap_pool_init(&ptr_smmu->bitmap_area, ptr_smmu->iova_byte,
                                 &ptr_mm_smmu->exclude_area);
    if (ret) {
        goto end;
    }
    ret = adlak_smmu_tlb_init(ptr_smmu, iova_max_size_GB);
    if (ret) {
        goto end;
    }

    AML_LOG_INFO("%s iova_max_size %uGB ,smmu_entry 0x%lX", __func__, iova_max_size_GB,
                 (uintptr_t)ptr_smmu->smmu_entry);
end:

    if (ret) {
        if (ptr_smmu) {
            adlak_os_free(ptr_smmu);
        }
        AML_LOG_ERR("%s fail!", __FUNCTION__);
    } else {
        // adlak_bitmap_dump(&ptr_smmu->bitmap_area);
        *pptr_smmu = ptr_smmu;
    }
    return ret;
}

static void adlak_smmu_destroy_obj(struct adlak_smmu_object **pptr_smmu) {
    struct adlak_smmu_object *ptr_smmu = *pptr_smmu;
    if (ptr_smmu) {
        AML_LOG_DEBUG("%s iova_byte 0x%lX", __func__, (uintptr_t)ptr_smmu->iova_byte);
        *pptr_smmu = NULL;
        adlak_smmu_tlb_deinit(ptr_smmu);
        adlak_bitmap_pool_deinit(&ptr_smmu->bitmap_area);
        adlak_os_free(ptr_smmu);
    }
}

static void adlak_smmu_init(void *ptr, uint8_t type) {
    struct adlak_smmu_object **pptr_smmu = ptr;
    int                        ret;

    AML_LOG_INFO("%s ,smmu tlb type %u", __func__, type);
    if (ADLAK_ENUM_SMMU_TLB_TYPE_PUBLIC_ONLY == type) {
        *pptr_smmu = ptr_mm_smmu->smmu_public;
    } else {
        adlak_smmu_claim_dev();
        // create smmu table for private
        ret = adlak_smmu_create_obj(pptr_smmu, ptr_mm_smmu->padlak->iova_max_size_GB);
        if (!ret) {
            list_add_tail(&((*pptr_smmu)->head), &ptr_mm_smmu->smmu_list);
        }
        adlak_smmu_release_dev();
    }
}

static void adlak_smmu_deinit(void *ptr, uint8_t type) {
    struct adlak_smmu_object **pptr_smmu = ptr;
    struct adlak_smmu_object * ptr_smmu = NULL, *next = NULL;
    AML_LOG_DEBUG("%s", __func__);
    if (ADLAK_ENUM_SMMU_TLB_TYPE_PUBLIC_ONLY != type) {
        adlak_smmu_claim_dev();
        list_for_each_entry_safe(ptr_smmu, next, &ptr_mm_smmu->smmu_list, head) {
            if (ptr_smmu == *pptr_smmu) {
                list_del(&ptr_smmu->head);
                adlak_smmu_destroy_obj(&ptr_smmu);
            }
        }
        adlak_smmu_release_dev();
    }
    *pptr_smmu = NULL;
}

static void adlak_smmu_get_usage(struct adlak_mem_usage *usage) {
    AML_LOG_DEBUG("%s", __func__);
    adlak_smmu_claim_dev();
    adlak_os_memcpy(usage, &ptr_mm_smmu->usage, sizeof(*usage));
    adlak_smmu_release_dev();
}

static uint64_t adlak_smmu_get_smmu_entry(struct adlak_context_smmu_attr *ctx_smmu_attr,
                                          uint8_t                         type) {
    struct adlak_smmu_object *ptr_smmu = NULL;

    AML_LOG_DEBUG("%s", __func__);
    if (ctx_smmu_attr == NULL) {
        ptr_smmu = ptr_mm_smmu->smmu_public;
    } else {
        if (type == ADLAK_ENUM_SMMU_TLB_TYPE_PRIVATE_ONLY) {
            ptr_smmu = ctx_smmu_attr->smmu_private;
        } else {
            ptr_smmu = ctx_smmu_attr->smmu_public;
        }
    }

    return ptr_smmu->smmu_entry;
}
static void adlak_smmu_invalidate(struct adlak_context_smmu_attr *ctx_smmu_attr) {
    struct adlak_smmu_object *smmu_public  = NULL;
    struct adlak_smmu_object *ptr_smmu_pri = NULL;
    struct adlak_device *     padlak       = ptr_mm_smmu->padlak;

    AML_LOG_INFO("%s", __func__);
    adlak_smmu_claim_dev();

    ptr_smmu_pri = ctx_smmu_attr->smmu_private;
    smmu_public  = ctx_smmu_attr->smmu_public;
    if (ctx_smmu_attr->smmu_tlb_updated) {
        ctx_smmu_attr->smmu_tlb_updated = 0;
        adlak_check_dev_is_idle(padlak);
        if (ptr_smmu_pri) {
            adlak_smmu_tlb_flush_cache(ptr_smmu_pri);
        }
        adlak_smmu_tlb_flush_cache(smmu_public);
    }
    adlak_smmu_release_dev();
}

static void adlak_mem_smmu_deinit(void) {
    struct adlak_smmu_object *ptr_smmu = NULL, *next = NULL;
    AML_LOG_DEBUG("%s", __func__);
    adlak_smmu_claim_dev();
    list_for_each_entry_safe(ptr_smmu, next, &ptr_mm_smmu->smmu_list, head) {
        list_del(&ptr_smmu->head);
        adlak_smmu_destroy_obj(&ptr_smmu);
    }
    adlak_smmu_release_dev();
    adlak_os_mutex_destroy(&ptr_mm_smmu->mutex);
}

static void adlak_mem_smmu_test(void) {
#if 0
    struct adlak_mm_pool_priv pool;
    struct adlak_mem_exclude  exclude_area = {0};
    uint64_t                  req_bytes, iova_byte, hole_bytes;
    uint64_t                  iova_addr, iova_addr_hole;
    AML_LOG_INFO("\n%s", __func__);
    AML_LOG_INFO("%s", "pool init");
    iova_byte = (4096 * 32 * 8) - (4096 * 2);
    adlak_bitmap_pool_init(&pool, iova_byte, &exclude_area);
    adlak_bitmap_dump(&pool);

    req_bytes = 4096 * 3;
    AML_LOG_INFO("alloc %lX byte", (uintptr_t)req_bytes);
    iova_addr = adlak_alloc_from_bitmap_pool(&pool, req_bytes);
    adlak_bitmap_dump(&pool);
    req_bytes = 4096 * 3;
    AML_LOG_INFO("alloc reverse %lX byte", (uintptr_t)req_bytes);
    iova_addr = adlak_alloc_from_bitmap_pool_reverse(&pool, req_bytes);
    adlak_bitmap_dump(&pool);
    hole_bytes = 4096 * 7;
    AML_LOG_INFO("alloc reverse %lX byte", (uintptr_t)hole_bytes);
    iova_addr_hole = adlak_alloc_from_bitmap_pool_reverse(&pool, hole_bytes);
    adlak_bitmap_dump(&pool);
    req_bytes = 4096 * 3;
    AML_LOG_INFO("alloc reverse %lX byte", (uintptr_t)req_bytes);
    iova_addr = adlak_alloc_from_bitmap_pool_reverse(&pool, req_bytes);
    adlak_bitmap_dump(&pool);

    AML_LOG_INFO("free start %lX ,size %lX byte", (uintptr_t)iova_addr_hole, (uintptr_t)hole_bytes);
    adlak_free_to_bitmap_pool(&pool, iova_addr_hole, hole_bytes);
    adlak_bitmap_dump(&pool);
    req_bytes = 4096 * 8;
    AML_LOG_INFO("alloc reverse %lX byte", (uintptr_t)req_bytes);
    iova_addr = adlak_alloc_from_bitmap_pool_reverse(&pool, req_bytes);
    adlak_bitmap_dump(&pool);
    req_bytes = 4096 * 6;
    AML_LOG_INFO("alloc reverse %lX byte", (uintptr_t)req_bytes);
    iova_addr = adlak_alloc_from_bitmap_pool_reverse(&pool, req_bytes);
    adlak_bitmap_dump(&pool);

    AML_LOG_INFO("\n%s end", __func__);
    while (1) {}
#endif
}

static int adlak_mem_smmu_init(struct adlak_device *padlak) {
    int                       ret;
    struct adlak_mem_exclude *exclude_area = &ptr_mm_smmu->exclude_area;
    AML_LOG_DEBUG("%s", __func__);
    INIT_LIST_HEAD(&ptr_mm_smmu->smmu_list);
    adlak_os_mutex_init(&ptr_mm_smmu->mutex);

    if (padlak->hw_res.adlak_sram_size) {
        exclude_area->start = padlak->hw_res.adlak_sram_pa;
        if (padlak->hw_res.sram_wrap) {
            exclude_area->size = padlak->hw_res.adlak_sram_size << 1;
        } else {
            exclude_area->size = padlak->hw_res.adlak_sram_size;
        }
    }

    ptr_mm_smmu->padlak = padlak;
    ptr_mm_smmu->mm.dev = padlak->dev;

    ret = adlak_smmu_create_obj(&ptr_mm_smmu->smmu_public, ptr_mm_smmu->padlak->iova_max_size_GB);
    if (!ret) {
        adlak_smmu_claim_dev();
        list_add_tail(&ptr_mm_smmu->smmu_public->head, &ptr_mm_smmu->smmu_list);
        adlak_smmu_release_dev();
    }
    ptr_mm_smmu->usage.pool_size = ptr_mm_smmu->smmu_public->iova_byte;
    adlak_mem_smmu_test();
    return ret;
}

void adlak_mem_smmu_unregister(struct adlak_device *padlak, struct adlak_mem_operator *ops) {
    AML_LOG_INFO("%s", __func__);
    if (ptr_mm_smmu) {
        adlak_mem_smmu_deinit();
        adlak_os_free(ptr_mm_smmu);
        ptr_mm_smmu = NULL;
    }
    ops->alloc               = NULL;
    ops->free                = NULL;
    ops->attach              = NULL;
    ops->dettach             = NULL;
    ops->mmap                = NULL;
    ops->unmap               = NULL;
    ops->vmap                = NULL;
    ops->vunmap              = NULL;
    ops->cache_flush         = NULL;
    ops->cache_invalid       = NULL;
    ops->smmu_init           = NULL;
    ops->smmu_deinit         = NULL;
    ops->smmu_tlb_invalidate = NULL;
    ops->smmu_tlb_dump       = NULL;
    ops->get_usage           = NULL;
    ops->get_smmu_entry      = NULL;
}

int adlak_mem_smmu_register(struct adlak_device *padlak, struct adlak_mem_operator *ops) {
    int ret = 0;
    adlak_mem_smmu_unregister(padlak, ops);
    AML_LOG_INFO("%s", __func__);
    if ((!ptr_mm_smmu)) {
        ptr_mm_smmu = adlak_os_zalloc(sizeof(struct adlak_mem_smmu), ADLAK_GFP_KERNEL);
        if (unlikely(!ptr_mm_smmu)) {
            ret = -1;
            goto end;
        }
    }

    ops->alloc               = adlak_smmu_alloc;
    ops->free                = adlak_smmu_free;
    ops->attach              = adlak_smmu_attach;
    ops->dettach             = adlak_smmu_dettach;
    ops->mmap                = adlak_smmu_mmap;
    ops->unmap               = adlak_smmu_unmap;
    ops->mmap_userspace      = adlak_smmu_mmap_userspace;
    ops->unmap_userspace     = adlak_smmu_unmap_userspace;
    ops->vmap                = adlak_smmu_vmap;
    ops->vunmap              = adlak_smmu_vunmap;
    ops->cache_flush         = adlak_smmu_cache_flush;
    ops->cache_invalid       = adlak_smmu_cache_invalid;
    ops->smmu_init           = adlak_smmu_init;
    ops->smmu_deinit         = adlak_smmu_deinit;
    ops->smmu_tlb_invalidate = adlak_smmu_invalidate;
    ops->smmu_tlb_dump       = adlak_smmu_tlb_dump;
    ops->get_usage           = adlak_smmu_get_usage;
    ops->get_smmu_entry      = adlak_smmu_get_smmu_entry;
    adlak_mem_smmu_init(padlak);

end:
    return ret;
}
