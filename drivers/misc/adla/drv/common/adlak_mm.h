/*******************************************************************************
 * Copyright (C) 2021 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm.h
 * @brief
 *
 * <pre>
 * MODIFICATION HISTORY:
 *
 * Ver   	Who				Date				Changes
 * ----------------------------------------------------------------------------
 * 1.00a shiwei.sun@amlogic.com	2021/06/05	Initial release
 * </pre>
 *
 ******************************************************************************/

#ifndef __ADLAK_MM_H__
#define __ADLAK_MM_H__

/***************************** Include Files *********************************/
#include "adlak_api.h"
#include "adlak_common.h"
#include "adlak_context.h"
#include "adlak_device.h"
#include "adlak_mm_common.h"
#ifdef __cplusplus
extern "C" {
#endif

/************************** Constant Definitions *****************************/

/**************************Global Variable************************************/

/**************************Type Definition and Structure**********************/

/************************** Function Prototypes ******************************/

/**************************Global Variable************************************/

/**************************Type Definition and Structure**********************/

int adlak_mem_alloc_request(struct adlak_context *context, struct adlak_buf_req *pbuf_req);
int adlak_mem_free_request(struct adlak_context *context, uint64_t mem_handle);
int adlak_mem_free_all_context(struct adlak_context *context);
int adlak_ext_mem_attach_request(struct adlak_context *        context,
                                 struct adlak_extern_buf_info *pbuf_req);
int adlak_ext_mem_dettach_request(struct adlak_context *context, uint64_t mem_handle);
int adlak_mem_flush_request(struct adlak_context *context, struct adlak_buf_flush *pflush_desc);

int adlak_mem_init(struct adlak_device *padlak);

void adlak_mem_deinit(struct adlak_device *padlak);

void adlak_mem_create_smmu(void *ptr_smmu, uint8_t type);

void adlak_mem_destroy_smmu(void *ptr_smmu, uint8_t type);

void adlak_mem_smmu_tlb_invalidate(struct adlak_context *context);

void adlak_mem_debug_smmu_tlb_dump(struct adlak_context *context);

uint64_t adlak_mem_get_smmu_entry(struct adlak_context *context, uint8_t type);

int adlak_flush_cache(struct adlak_device *padlak, struct adlak_mem_handle *mm_info,
                      struct adlak_sync_cache_ext_info *sync_cache_ext_info);

int adlak_invalid_cache(struct adlak_device *padlak, struct adlak_mem_handle *mm_info,
                        struct adlak_sync_cache_ext_info *sync_cache_ext_info);

int adlak_mem_mmap(struct adlak_context *context, void *const vma, uint64_t iova);

void *adlak_mem_vmap(struct adlak_mem_handle *mm_info);

int adlak_mem_get_usage(struct adlak_mem_usage *usage);

void adlak_mem_usage_update(struct adlak_device *padlak);

struct adlak_mem_handle *adlak_cmq_buf_alloc(struct adlak_context *context, size_t size);

void adlak_cmq_buf_free(struct adlak_context *context, struct adlak_mem_handle *cmq_mm_info);

#ifdef __cplusplus
}
#endif

#endif /* __ADLAK_MM_H__ end define*/
