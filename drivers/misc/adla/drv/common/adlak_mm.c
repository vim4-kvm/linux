/*******************************************************************************
 * Copyright (C) 2021 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm.c
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

/***************************** Include Files *********************************/
#include "adlak_mm.h"

#include "adlak_api.h"
#include "adlak_common.h"
#include "adlak_context.h"
#include "adlak_device.h"
#include "adlak_mm_cma.h"
#include "adlak_mm_common.h"
#include "adlak_mm_mbd.h"
#include "adlak_mm_os_common.h"

#ifndef CONFIG_ADLA_FREERTOS
#include "adlak_mm_smmu.h"
#endif

/************************** Constant Definitions *****************************/

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Variable Definitions *****************************/

static struct adlak_mem_obj memory = {
    .use_smmu = 0,
    .use_mbd  = 0,
};
/************************** Function Prototypes ******************************/
void adlak_public_cmq_buf_deinit(struct adlak_device *padlak);
int  adlak_public_cmq_buf_init(struct adlak_device *padlak);

/************************** Function Prototypes ******************************/

int adlak_mem_init(struct adlak_device *padlak) {
    int                   ret = -1;
    struct adlak_mem_obj *mem = &memory;
    ret                       = adlak_mem_uid_init();
    if (ret) {
        return ret;
    }

    mem->use_smmu = padlak->smmu_en;
    if (mem->use_smmu) {
#ifndef CONFIG_ADLA_FREERTOS
        ret = adlak_mem_smmu_register(padlak, &mem->ops);
#endif
    } else

    {
        if (mem->use_mbd) {
            ret = adlak_mem_mbd_register(padlak, &mem->ops);
        } else {
            ret = adlak_mem_cma_register(padlak, &mem->ops);
        }
    }
    if (ret) {
        AML_LOG_ERR("mem ops register fail!");
        return ret;
    }
    ret = adlak_public_cmq_buf_init(padlak);
    if (ret) {
        AML_LOG_ERR("cmq buf init fail!");
        adlak_mem_deinit(padlak);
    }
    AML_LOG_INFO("mem init success");
    return ret;
}

void adlak_mem_deinit(struct adlak_device *padlak) {
    struct adlak_mem_obj *mem = &memory;

    AML_LOG_DEBUG("%s", __func__);
    adlak_public_cmq_buf_deinit(padlak);
    if (mem->use_smmu) {
#ifndef CONFIG_ADLA_FREERTOS
        adlak_mem_smmu_unregister(padlak, &mem->ops);
#endif
    } else {
        if (mem->use_mbd) {
            adlak_mem_mbd_unregister(padlak, &mem->ops);
        } else {
            adlak_mem_cma_unregister(padlak, &mem->ops);
        }
    }
    adlak_mem_uid_deinit();
    AML_LOG_INFO("mem deinit success");
}

int adlak_mem_alloc_request(struct adlak_context *context, struct adlak_buf_req *pbuf_req) {
    int                      ret     = 0;
    struct adlak_mem_handle *mm_info = NULL;
    struct adlak_mem_obj *   mem     = &memory;
    AML_LOG_DEBUG("%s", __func__);
    AML_LOG_DEBUG("mem_alloc_request size:0x%lX bytes", (uintptr_t)pbuf_req->bytes);

    // step1: allocate mem
    mm_info = mem->ops.alloc(&context->smmu_attr, pbuf_req);

    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        ret = ERR(ENOMEM);
        AML_LOG_ERR("adlak_dma_alloc_and_map failed.");
        goto err_malloc;
    }
    mm_info->uid = mm_info->uid | context->net_id;
    // step2: alloc_usermap
    if (pbuf_req->mmap_en) {
        ret = mem->ops.mmap_userspace(mm_info);
        if (ret) {
            AML_LOG_ERR("adlak_dma_mmap2userspace failed.");
            goto err_mmap;
        }
    }
    AML_LOG_DEBUG("cpu_addr_user=0x%lX, ", (uintptr_t)mm_info->cpu_addr_user);
    AML_LOG_DEBUG("iova_addr=0x%lX, ", (uintptr_t)mm_info->iova_addr);
    AML_LOG_DEBUG("size=%lu Kbytes\n", (uintptr_t)(mm_info->req.bytes / 1024));
    pbuf_req->ret_desc.bytes = mm_info->req.bytes;

    pbuf_req->ret_desc.va_user   = (uint64_t)(uintptr_t)mm_info->cpu_addr_user;
    pbuf_req->ret_desc.iova_addr = (uint64_t)(uintptr_t)mm_info->iova_addr;
    pbuf_req->ret_desc.phys_addr = (uint64_t)(uintptr_t)mm_info->phys_addr;
    pbuf_req->mem_handle         = (uint64_t)(uintptr_t)mm_info;
    pbuf_req->ret_desc.uid       = mm_info->uid;

    // step3: attach to the mem list
    if (ERR(NONE) == ret) {
        ret = adlak_context_attach_buf(context, (void *)mm_info);
        if (ERR(NONE) != ret) {
            AML_LOG_ERR("attach mm_info to context failed!");
            goto err_attach;
        }
    }

    // update the usage of memory
    if (0 == (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE)) {
        context->mem_alloced += mm_info->req.bytes;
    }

    return ERR(NONE);
err_attach:

    mem->ops.unmap_userspace(mm_info);
err_mmap:
    mem->ops.free(&context->smmu_attr, mm_info);
err_malloc:
    return ret;
}

int adlak_mem_free_request(struct adlak_context *context, uint64_t mem_handle) {
    int                      ret     = 0;
    struct adlak_mem_handle *mm_info = (struct adlak_mem_handle *)(uintptr_t)mem_handle;
    struct adlak_mem_handle *pmm_info_hd;
    struct adlak_mem_obj *   mem = &memory;
    AML_LOG_DEBUG("%s", __func__);
    if (0 != context->invoke_cnt) {
        ret = -1;
        goto err;
    }

    // step1: dettach from the mem list
    pmm_info_hd = (struct adlak_mem_handle *)adlak_context_dettach_buf(context, (void *)mm_info);
    if (NULL == pmm_info_hd) {
        ret = -1;
        AML_LOG_ERR("dettach mm_info to context failed!");
        goto err;
    }
    AML_LOG_DEFAULT("iova_addr=0x%lX, ", (uintptr_t)mm_info->iova_addr);
    AML_LOG_DEFAULT("size=%lu KByte \n", (uintptr_t)(mm_info->req.bytes / 1024));

    // step2: free_usermap
    if (mm_info->cpu_addr_user) {
        mem->ops.unmap_userspace(mm_info);
    }

    // update the usage of memory
    if (0 == (mm_info->mem_type & ADLAK_ENUM_MEMTYPE_INNER_SHARE)) {
        context->mem_alloced -= mm_info->req.bytes;
    }

    // step3: free mem
    mem->ops.free(&context->smmu_attr, mm_info);
    return 0;
err:
    return ret;
}

int adlak_ext_mem_attach_request(struct adlak_context *        context,
                                 struct adlak_extern_buf_info *pbuf_req) {
    int                      ret     = 0;
    struct adlak_mem_handle *mm_info = NULL;
    struct adlak_mem_obj *   mem     = &memory;
    AML_LOG_DEBUG("%s", __func__);
    if (ADLAK_PAGE_ALIGN(pbuf_req->bytes) != pbuf_req->bytes) {
        ret = -1;
        AML_LOG_ERR("mem size is not align with page_size.");
        goto err;
    }

    // step1: attach extern buffer
    if (0 == pbuf_req->buf_type)  // physical addr type
    {
        AML_LOG_DEBUG("this extern memory type is physical.");

        mm_info = mem->ops.attach(&context->smmu_attr, pbuf_req);
    } else if (1 == pbuf_req->buf_type)  //  dma handle addr type
    {
        //  AML_LOG_DEBUG("this extern memory type is dma handle.");
        ret = -1;
        AML_LOG_ERR("This memory type[%u] is unsupported Temporarily.", pbuf_req->buf_type);
        goto err;

    } else {
        ret = -1;
        AML_LOG_ERR("This memory type[%u] is not support.", pbuf_req->buf_type);
        goto err;
    }

    if (ADLAK_IS_ERR_OR_NULL(mm_info)) {
        ret = -1;
        AML_LOG_ERR("adlak_dma_alloc_and_map failed.");
        goto err;
    }
    mm_info->uid = mm_info->uid | context->net_id;

    mm_info->cpu_addr_user   = NULL;
    pbuf_req->ret_desc.bytes = mm_info->req.bytes;

    pbuf_req->ret_desc.va_user   = (uint64_t)(uintptr_t)mm_info->cpu_addr_user;
    pbuf_req->ret_desc.iova_addr = (uint64_t)(uintptr_t)mm_info->iova_addr;
    pbuf_req->mem_handle         = (uint64_t)(uintptr_t)mm_info;
    pbuf_req->ret_desc.uid       = mm_info->uid;

    // step2: alloc_usermap
    if (pbuf_req->mmap_en) {
        ret = mem->ops.mmap_userspace(mm_info);
        if (ret) {
            AML_LOG_ERR("adlak_dma_mmap2userspace failed.");
            goto err_mmap;
        }
    }
    AML_LOG_DEBUG("cpu_addr_user=0x%lX, ", (uintptr_t)mm_info->cpu_addr_user);
    AML_LOG_DEBUG("iova_addr=0x%lX, ", (uintptr_t)mm_info->iova_addr);
    AML_LOG_DEBUG("size=%lu Kbytes\n", (uintptr_t)(mm_info->req.bytes / 1024));
    pbuf_req->ret_desc.bytes = mm_info->req.bytes;

    pbuf_req->ret_desc.va_user   = (uint64_t)(uintptr_t)mm_info->cpu_addr_user;
    pbuf_req->ret_desc.iova_addr = (uint64_t)(uintptr_t)mm_info->iova_addr;
    pbuf_req->ret_desc.phys_addr = (uint64_t)(uintptr_t)mm_info->phys_addr;

    pbuf_req->errcode = 0;

    // step3: attach to the mem list
    if (ERR(NONE) == ret) {
        ret = adlak_context_attach_buf(context, (void *)mm_info);
        if (ERR(NONE) != ret) {
            AML_LOG_ERR("attach mm_info to context failed!");
        }
    }
    return 0;
err_mmap:
    mem->ops.dettach(&context->smmu_attr, mm_info);
err:

    return ret;
}

int adlak_ext_mem_dettach_request(struct adlak_context *context, uint64_t mem_handle) {
    int                      ret     = 0;
    struct adlak_mem_handle *mm_info = (struct adlak_mem_handle *)(uintptr_t)mem_handle;
    struct adlak_mem_handle *pmm_info_hd;
    struct adlak_mem_obj *   mem = &memory;
    AML_LOG_DEBUG("%s", __func__);

    // step1: dettach from the tlb of smmu
    pmm_info_hd = (struct adlak_mem_handle *)adlak_context_dettach_buf(context, (void *)mm_info);
    if (NULL == pmm_info_hd) {
        AML_LOG_ERR("dettach mm_info to context failed!");
        ret = -1;
        goto err;
    }

    AML_LOG_DEFAULT("iova_addr=0x%lX, ", (uintptr_t)mm_info->iova_addr);
    AML_LOG_DEFAULT("size=%lu KByte \n", (uintptr_t)(mm_info->req.bytes / 1024));
    // step2: free_usermap
    if (mm_info->cpu_addr_user) {
        mem->ops.unmap_userspace(mm_info);
    }

    // step3: dettach from mem list
    mem->ops.dettach(&context->smmu_attr, mm_info);
    return 0;
err:
    return ret;
}

int adlak_mem_free_all_context(struct adlak_context *context) {
    struct adlak_mem_handle *pmm_info_hd;
    struct context_buf *     pcontext = NULL, *pcontext_tmp = NULL;
    struct adlak_mem_obj *   mem = &memory;
    AML_LOG_DEBUG("%s", __func__);
    if (!list_empty(&context->sbuf_list)) {
        list_for_each_entry_safe(pcontext, pcontext_tmp, &context->sbuf_list, head) {
            if (pcontext) {
                list_del(&pcontext->head);
                pmm_info_hd = pcontext->mm_info;
                if (CONTEXT_STATE_CLOSED != context->state) {
                    // not need this.  adlak_dma_unmmap_userspace
                }
                if (pmm_info_hd->mem_src != ADLAK_ENUM_MEMSRC_EXT_PHYS) {
                    context->mem_alloced -= pmm_info_hd->req.bytes;
                    mem->ops.free(&context->smmu_attr, pmm_info_hd);
                } else {
                    mem->ops.dettach(&context->smmu_attr, pmm_info_hd);
                }
                adlak_os_free(pcontext);
            }
        }
    }
    return 0;
}

int adlak_mem_flush_request(struct adlak_context *context, struct adlak_buf_flush *pflush_desc) {
    int                              ret        = 0;
    struct adlak_mem_handle *        mm_info    = NULL;
    struct adlak_device *            padlak     = context->padlak;
    struct context_buf *             target_buf = NULL;
    struct adlak_sync_cache_ext_info sync_cache_extern;

    AML_LOG_DEBUG("%s", __func__);
    mm_info = (struct adlak_mem_handle *)(uintptr_t)(pflush_desc->mem_handle);

    sync_cache_extern.is_partial = pflush_desc->is_partial;
    sync_cache_extern.offset     = pflush_desc->offset;
    sync_cache_extern.size       = pflush_desc->size;
    /* LOCK */
    adlak_os_mutex_lock(&context->context_mutex);
    target_buf = find_buffer_by_desc(context, (void *)mm_info);
    if (!target_buf) {
        AML_LOG_ERR("no corresponding buffer found in this context!");
        ret = -1;
    } else {
        switch (pflush_desc->direction) {
            case FLUSH_TO_DEVICE:
                // flush cache
                adlak_flush_cache(padlak, mm_info, &sync_cache_extern);
                break;
            case FLUSH_FROM_DEVICE:
                // invalid cache
                adlak_invalid_cache(padlak, mm_info, &sync_cache_extern);
                break;
            default:
                break;
        }
        target_buf = NULL;
        ret        = 0;
    }
    /* UNLOCK */
    adlak_os_mutex_unlock(&context->context_mutex);

    return ret;
}

struct adlak_mem_handle *adlak_cmq_buf_alloc(struct adlak_context *context, size_t size) {
    struct adlak_mem_handle *cmq_mm_info = NULL;
    struct adlak_buf_req     buf_req;
    struct adlak_mem_obj *   mem = &memory;

    if (size > 0x10000000) {
        AML_LOG_ERR("allocate cmq size %08lX grater than 256Mbytes", (uintptr_t)size);
        return cmq_mm_info;
    }
    buf_req.bytes    = size;
    buf_req.mem_type = ADLAK_ENUM_MEMTYPE_INNER;
    /*In order to avoid constantly sync the cache of cmq, it is recommended that cmq use
     * uncache buffer*/
    //  buf_req.ret_desc.mem_type      = buf_req.ret_desc.mem_type | ADLAK_ENUM_MEMTYPE_CACHEABLE;
    buf_req.mem_direction = ADLAK_ENUM_MEM_DIR_WRITE_ONLY;
    if (context) {
        cmq_mm_info = mem->ops.alloc(&context->smmu_attr, &buf_req);
    } else {
        cmq_mm_info = mem->ops.alloc(NULL, &buf_req);
    }
    if (NULL == cmq_mm_info) {
        AML_LOG_ERR("alloc buffer for cmq failed!");
    } else {
        AML_LOG_INFO("cmq buffer iova_addr=0x%lX", (uintptr_t)cmq_mm_info->iova_addr);
    }
    return cmq_mm_info;
}

void adlak_cmq_buf_free(struct adlak_context *context, struct adlak_mem_handle *cmq_mm_info) {
    struct adlak_mem_obj *mem = &memory;
    if (NULL != cmq_mm_info) {
        if (context) {
            mem->ops.free(&context->smmu_attr, cmq_mm_info);
        } else {
            mem->ops.free(NULL, cmq_mm_info);
        }
    }
}

int adlak_public_cmq_buf_init(struct adlak_device *padlak) {
    struct adlak_cmq_buffer *cmq_buf_info = &padlak->cmq_buffer_public;
    AML_LOG_DEBUG("%s", __func__);
    AML_LOG_INFO("alloc buffer for cmq(public),size=0x%08x", cmq_buf_info->size);
    cmq_buf_info->cmq_mm_info = adlak_cmq_buf_alloc(NULL, cmq_buf_info->size);
    if (NULL == cmq_buf_info->cmq_mm_info) {
        AML_LOG_ERR("alloc buffer for cmq(public) failed!");
        goto err;
    }
    cmq_buf_info->cmq_wr_offset = 0;
    cmq_buf_info->cmq_rd_offset = 0;
    return 0;
err:
    return -1;
}

void adlak_public_cmq_buf_deinit(struct adlak_device *padlak) {
    struct adlak_cmq_buffer *cmq_buf_info = &padlak->cmq_buffer_public;
    AML_LOG_DEBUG("%s", __func__);
    cmq_buf_info->cmq_wr_offset = 0;
    cmq_buf_info->cmq_rd_offset = 0;
    adlak_cmq_buf_free(NULL, cmq_buf_info->cmq_mm_info);
}

int adlak_mem_mmap(struct adlak_context *context, void *const vma, uint64_t uid) {
    int ret = 0;

    struct adlak_mem_handle  mm_info;
    struct adlak_mem_handle *pmm_info_hd;
    struct context_buf *     target_buf = NULL;
    struct adlak_mem_obj *   mem        = &memory;
    AML_LOG_DEBUG("%s", __func__);
    mm_info.uid = uid | context->net_id;

    /* LOCK */
    adlak_os_mutex_lock(&context->context_mutex);
    target_buf = find_buffer_by_desc(context, &mm_info);
    if (!target_buf) {
        AML_LOG_ERR("no corresponding buffer found in this context!");
        ret = -1;
        goto err;
    } else {
        pmm_info_hd = target_buf->mm_info;
    }

    ret = mem->ops.mmap(pmm_info_hd, vma);
    if (ret) {
        AML_LOG_ERR("adlak_dma_map failed.");
        goto err;
    }
err:
    adlak_os_mutex_unlock(&context->context_mutex);

    return ret;
}

int adlak_mem_get_usage(struct adlak_mem_usage *usage) {
    struct adlak_mem_obj *mem = &memory;
    mem->ops.get_usage(usage);
    return 0;
}

void adlak_mem_create_smmu(void *pptr_smmu, uint8_t type) {
    struct adlak_mem_obj *mem = &memory;
    if (mem->use_smmu) {
        mem->ops.smmu_init(pptr_smmu, type);
    }
}

void adlak_mem_destroy_smmu(void *pptr_smmu, uint8_t type) {
    struct adlak_mem_obj *mem = &memory;
    if (mem->use_smmu) {
        mem->ops.smmu_deinit(pptr_smmu, type);
    }
}

void adlak_mem_smmu_tlb_invalidate(struct adlak_context *context) {
    struct adlak_mem_obj *mem = &memory;
    if (mem->use_smmu) {
        mem->ops.smmu_tlb_invalidate(&context->smmu_attr);
    }
}

void adlak_mem_debug_smmu_tlb_dump(struct adlak_context *context) {
    struct adlak_mem_obj *mem = &memory;
    if (mem->use_smmu) {
        if (context) {
            return mem->ops.smmu_tlb_dump(&context->smmu_attr);
        }
    }
}

int adlak_flush_cache(struct adlak_device *padlak, struct adlak_mem_handle *mm_info,
                      struct adlak_sync_cache_ext_info *sync_cache_ext_info) {
    struct adlak_mem_obj *mem = &memory;
    return mem->ops.cache_flush(mm_info, sync_cache_ext_info);
}

int adlak_invalid_cache(struct adlak_device *padlak, struct adlak_mem_handle *mm_info,
                        struct adlak_sync_cache_ext_info *sync_cache_ext_info) {
    struct adlak_mem_obj *mem = &memory;
    return mem->ops.cache_invalid(mm_info, sync_cache_ext_info);
}

void *adlak_mem_vmap(struct adlak_mem_handle *mm_info) {
    struct adlak_mem_obj *mem = &memory;
    return mem->ops.vmap(mm_info);
}

void adlak_mem_usage_update(struct adlak_device *padlak) {
    struct adlak_caps_desc *uapi_caps = (struct adlak_caps_desc *)padlak->dev_caps.data;
    struct adlak_mem_usage  mem_usage;
    struct adlak_mem_obj *  mem = &memory;
    adlak_mem_get_usage(&mem_usage);
    if (mem->use_smmu) {
        uapi_caps->hw_iova_max_size =
            0x100000000; /* tha maximum vaddr value allowed by the hardware*/
        uapi_caps->iova_max_size = mem_usage.pool_size; /* tha maximum vaddr of smmu*/
        uapi_caps->iova_free_size =
            mem_usage.pool_size -
            (mem_usage.alloced_umd + mem_usage.alloced_kmd); /* tha free size of vaddr*/
    }
}

uint64_t adlak_mem_get_smmu_entry(struct adlak_context *context, uint8_t type) {
    struct adlak_mem_obj *mem = &memory;
    if (mem->use_smmu) {
        if (NULL == context) {
            return mem->ops.get_smmu_entry(NULL, type);
        } else {
            return mem->ops.get_smmu_entry(&context->smmu_attr, type);
        }
    } else {
        return ADLAK_INVALID_ADDR;
    }
}
