/*******************************************************************************
 * Copyright (C) 2021 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_platform_config.c
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
#include "adlak_platform_config.h"

#include "adlak_common.h"
#include "adlak_device.h"
#include "adlak_dpm.h"
#include "adlak_hw.h"
#include "adlak_interrupt.h"
#include "adlak_submit.h"

/************************** Constant Definitions *****************************/
#ifndef CONFIG_OF

static struct platform_device *pdev = NULL;
#endif

static uint irqline = 0;

static uint registerMemBase = 0;

static uint registerMemSize = 0;

static uint contiguousMemBase = 0;

static uint contiguousMemSize = 0;

static uint contiguousSramBase = 0;

static uint contiguousSramSize = 0;

static int adlak_has_smmu = -1;

static int adlak_dependency_mode = ADLAK_DEPENDENCY_MODE_MODULE_LAYER;

static int adlak_axi_freq = 800000000;

static int adlak_core_freq = 800000000;

static int adlak_cmd_queue_size = 1024 * 1024;

static int adlak_sch_time_max_ms = 10;

static int adlak_dpm_period = 300;

static int adlak_log_level = -1;

static uint adlak_share_swap = 0;

static uint adlak_share_buf_size = 0;

static uint adlak_smmu_iova_size = 2;

#include "./adlak_platform_module_param.c"
/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/

/************************** Variable Definitions *****************************/
#if CONFIG_ADLAK_EMU_EN
extern uint32_t g_adlak_emu_dev_cmq_total_size;
#endif

/************************** Function Prototypes ******************************/

#ifndef CONFIG_OF

static struct resource adlak_resource[] = {

    [0] =
        {
            .name  = "adla_reg",
            .start = ADLAK_REG_PHY_ADDR,
            .end   = ADLAK_REG_PHY_ADDR + (ADLAK_REG_SIZE - 1),
            .flags = IORESOURCE_MEM,
        },
    [1] =
        {
            .name  = "adla_reserved_memory",
            .start = ADLAK_MEM_PHY_ADDR,
            .end   = ADLAK_MEM_PHY_ADDR + (ADLAK_MEM_SIZE - 1),
            .flags = IORESOURCE_MEM,
        },

    [2] =
        {
            .name  = "adla",
            .start = ADLAK_IRQ_LINE,
            .end   = ADLAK_IRQ_LINE,
            .flags = IORESOURCE_IRQ,
        },
    [3] =
        {
            .name  = "adla_sram",
            .start = ADLAK_SRAM_PHY_ADDR,
            .end   = ADLAK_SRAM_PHY_ADDR + (ADLAK_SRAM_SIZE - 1),
            .flags = IORESOURCE_MEM,
        },

};

static void platform_adlak_device_release(struct device *dev) { return; }

static uint64_t               adlak_dev_dmamask = DMA_BIT_MASK(34);
static struct platform_device adlak_pdev        = {
    .name          = DEVICE_NAME,
    .id            = 0,
    .resource      = adlak_resource,
    .num_resources = ADLAK_ARRAY_SIZE(adlak_resource),
    .dev =
        {
            .release           = platform_adlak_device_release,
            .dma_mask          = &adlak_dev_dmamask,
            .coherent_dma_mask = DMA_BIT_MASK(34),
        },
};

static void adlak_drv_show_param(void) {
    AML_LOG_DEBUG("%s", __func__);
    AML_LOG_DEFAULT("");
    AML_LOG_DEFAULT("registerMemBase         0x%08llX, ", (uint64_t)adlak_resource[0].start);
    AML_LOG_DEFAULT("registerMemSize         0x%08llX, ",
                    (uint64_t)(adlak_resource[0].end + 1 - adlak_resource[0].start));
    AML_LOG_DEFAULT("contiguousMemBase       0x%08llX, ", (uint64_t)adlak_resource[1].start);
    AML_LOG_DEFAULT("contiguousMemSize       0x%08llX, ",
                    (uint64_t)(adlak_resource[1].end + 1 - adlak_resource[1].start));
    AML_LOG_DEFAULT("irqline                 0x%08llX, ", (uint64_t)adlak_resource[2].start);
    AML_LOG_DEFAULT("contiguousSramBase      0x%08llX, ", (uint64_t)adlak_resource[3].start);
    AML_LOG_DEFAULT("contiguousSramSize      0x%08llX, ",
                    (uint64_t)(adlak_resource[3].end + 1 - adlak_resource[3].start));
    AML_LOG_DEFAULT("\n");
}

static void adlak_update_commandline_parameters(void) {
    AML_LOG_DEBUG("%s", __func__);
    if (0 != registerMemBase) {
        adlak_resource[0].start = registerMemBase;
        if (0 == registerMemSize) {
            registerMemSize = ADLAK_REG_SIZE;
        }
        adlak_resource[0].end = registerMemBase + registerMemSize - 1;
    }
    if (0 != contiguousMemBase) {
        adlak_resource[1].start = contiguousMemBase;
        if (0 == contiguousMemSize) {
            contiguousMemSize = ADLAK_MEM_SIZE;
        }
        adlak_resource[1].end = contiguousMemBase + contiguousMemSize - 1;
    }
    if (0 != irqline) {
        adlak_resource[2].start = irqline;
        adlak_resource[2].end   = adlak_resource[2].start;
    }
    if (0 != contiguousSramBase) {
        adlak_resource[3].start = contiguousSramBase;
        if (0 == contiguousSramSize) {
            contiguousSramSize = ADLAK_SRAM_SIZE;
        }
        adlak_resource[3].end = contiguousSramBase + contiguousSramSize - 1;
    }
    adlak_drv_show_param();
}
#endif
int adlak_platform_device_init(void) {
    int ret = 0;
    AML_LOG_DEBUG("%s", __func__);
#ifndef CONFIG_OF
    pdev = &adlak_pdev;
    adlak_update_commandline_parameters();
    platform_device_register(pdev);
#endif
    return ret;
}

int adlak_platform_device_uninit(void) {
    int ret = 0;
    AML_LOG_DEBUG("%s", __func__);

#ifndef CONFIG_OF
    if (pdev) {
        platform_device_unregister(pdev);
        pdev = NULL;
    }
    AML_LOG_DEBUG("%s End", __func__);
#else

#endif
    return ret;
}

int adlak_platform_get_resource(void *data) {
    int                  ret    = 0;
    struct resource *    res    = NULL;
    struct adlak_device *padlak = (struct adlak_device *)data;
    u32  adla_core_clk_rate = 0;

    AML_LOG_DEBUG("%s", __func__);

	padlak->smmu_en = true;

    if (padlak->smmu_en) {
        AML_LOG_INFO("smmu available.\n");
    } else {
        AML_LOG_INFO("smmu not available.\n");
    }

    /* get ADLAK IO */

    res = platform_get_resource_byname(padlak->pdev, IORESOURCE_MEM, "adla_reg");
    if (!res) {
        AML_LOG_ERR("get platform io region failed");
        ret = ERR(EINVAL);
        goto err;
    }
    AML_LOG_DEBUG("get ADLAK IO region: [0x%lX, 0x%lX]", (uintptr_t)res->start,
                  (uintptr_t)res->end);

    padlak->hw_res.adlak_reg_pa   = res->start;
    padlak->hw_res.adlak_reg_size = res->end - res->start + 1;

    res = platform_get_resource_byname(padlak->pdev, IORESOURCE_MEM, "adla_sram");
    if (!res) {
        AML_LOG_INFO("get platform sram region failed");
        padlak->hw_res.adlak_sram_pa   = 0;
        padlak->hw_res.adlak_sram_size = 0;
    } else {
        AML_LOG_DEBUG("get ADLAK SRAM region: [0x%lX, 0x%lX]", (uintptr_t)res->start,
                      (uintptr_t)res->end);
        padlak->hw_res.adlak_sram_pa   = res->start;
        padlak->hw_res.adlak_sram_size = res->end - res->start + 1;
    }
    padlak->hw_res.sram_wrap = 1;  // this configure must sync with adla-compiler,the default is
                                   // wrap enable in adla-compiler.

    /* get reserve-memory */

    res = platform_get_resource_byname(padlak->pdev, IORESOURCE_MEM, "adla_reserved_memory");
    if (!res) {
        AML_LOG_INFO("get platform reserved_memory region failed");
        padlak->hw_res.adlak_resmem_pa   = 0;
        padlak->hw_res.adlak_resmem_size = 0;
    } else {
        AML_LOG_DEBUG("get ADLA reserved_memory region: [0x%lX, 0x%lX]", (uintptr_t)res->start,
                      (uintptr_t)res->end);
        padlak->hw_res.adlak_resmem_pa   = res->start;
        padlak->hw_res.adlak_resmem_size = res->end - res->start + 1;
    }

    /* get interrupt number */
	ret = platform_get_irq_byname(padlak->pdev, "adla");
	if (ret < 0)
		return ret;
	padlak->hw_res.irqline = ret;
    AML_LOG_DEBUG("get IRQ number: %d", padlak->hw_res.irqline);

    padlak->hw_timeout_ms = (adlak_sch_time_max_ms);
    AML_LOG_DEBUG("padlak->hw_timeout_ms =  %d ms", adlak_sch_time_max_ms);

    padlak->cmq_buffer_public.size = ADLAK_ALIGN(adlak_cmd_queue_size, 256);
#if CONFIG_ADLAK_EMU_EN
    g_adlak_emu_dev_cmq_total_size = padlak->cmq_buffer_public.size;
#endif
    AML_LOG_DEBUG("cmq_size=%d byte", padlak->cmq_buffer_public.size);

    if (adlak_dependency_mode >= ADLAK_DEPENDENCY_MODE_COUNT) {
        adlak_dependency_mode = ADLAK_DEPENDENCY_MODE_MODULE_H_COUNT;
    }
    padlak->dependency_mode = adlak_dependency_mode;
    AML_LOG_DEBUG("padlak->dependency_mode =  %d", padlak->dependency_mode);

	/* T7C shares the core clock with AXI; no separate AXI clock input. */
	padlak->clk_axi = NULL;
    padlak->clk_core = devm_clk_get(padlak->dev, "adla_core_clk");
	if (IS_ERR(padlak->clk_core))
		return PTR_ERR(padlak->clk_core);
#ifdef CONFIG_OF
    of_property_read_u32(padlak->dev->of_node, "adla_core_clk_rate", &adla_core_clk_rate);
#endif

    if (adla_core_clk_rate != 0) {
        padlak->clk_axi_freq_set  = adla_core_clk_rate;
        padlak->clk_core_freq_set = adla_core_clk_rate;
    } else {
        padlak->clk_axi_freq_set  = adlak_axi_freq;
        padlak->clk_core_freq_set = adlak_core_freq;
    }

    /*
        if clk tree power off,it will switch adla_clk to adla_clk_parent1 auto;
        adla_core_clk -->adla_clk -->adla_clk_parent0/adla_clk_parent1;
    */
    padlak->clk = devm_clk_get(padlak->dev, "adla_clk");
	if (IS_ERR(padlak->clk))
		return PTR_ERR(padlak->clk);
    padlak->clk_parent0 = devm_clk_get(padlak->dev, "adla_clk_parent0");
	if (IS_ERR(padlak->clk_parent0))
		return PTR_ERR(padlak->clk_parent0);
    padlak->clk_parent1 = devm_clk_get(padlak->dev, "adla_clk_parent1");
	if (IS_ERR(padlak->clk_parent1))
		return PTR_ERR(padlak->clk_parent1);

    padlak->dpm_period_set    = adlak_dpm_period;

    if (adlak_log_level != -1) {
        g_adlak_log_level = adlak_log_level;
#if ADLAK_DEBUG
        g_adlak_log_level_pre = g_adlak_log_level;
#endif
    }

    padlak->share_swap_en  = 0;
    padlak->share_buf_size = 0;
    if (adlak_share_swap > 0) {
        padlak->share_swap_en  = 1;
        padlak->share_buf_size = adlak_share_buf_size;
    }
    if (adlak_smmu_iova_size > 4) {
        padlak->iova_max_size_GB = 4;
    } else {
        padlak->iova_max_size_GB = adlak_smmu_iova_size;
    }

    return 0;
err:
    return ret;
}

int adlak_platform_get_rsv_mem_size(void *dev, uint64_t *mem_size) {
    int                 ret = 0;
    struct resource     res;
    uint64_t            size;
    const __be32 *      ranges = NULL;
    int                 nsize;
    struct device_node *res_mem_dev;
    /* find a memory-region phandle */
    res_mem_dev = of_parse_phandle(((struct device *)dev)->of_node, "memory-region", 0);
    if (!res_mem_dev) {
        goto err;
    }
    ret = of_address_to_resource(res_mem_dev, 0, &res);
    if (!ret) {
        AML_LOG_DEBUG("get cma memory region: [0x%lX, 0x%lX]", (uintptr_t)res.start,
                      (uintptr_t)res.end);
        size = res.end - res.start + 1;
    } else {
        nsize  = of_n_size_cells(res_mem_dev);
        ranges = of_get_property(res_mem_dev, "size", NULL);
        if (!ranges) {
            AML_LOG_ERR("get cma size failed!\n");
            goto err;
        }
        size = of_read_number(ranges, nsize);
    }
    AML_LOG_DEBUG("get cma size=0x%lX", (uintptr_t)size);
    *mem_size = size;
    return 0;
err:
    return -1;
}

int adlak_platform_request_resource(void *data) {
    int                  ret    = 0;
    struct adlak_device *padlak = (struct adlak_device *)data;

    AML_LOG_DEBUG("%s", __func__);
    padlak->hw_res.preg = adlak_create_ioregion((uintptr_t)padlak->hw_res.adlak_reg_pa,
                                                padlak->hw_res.adlak_reg_size);
    if (NULL == padlak->hw_res.preg) {
        AML_LOG_ERR("create ioregion failed");
        ret = ERR(EINVAL);
        goto err;
    }

    padlak->all_task_num     = 0;
    padlak->all_task_num_max = ADLAK_TASK_COUNT_MAX;

    return 0;
err:
    return ret;
}

int adlak_platform_free_resource(void *data) {
    int                  ret    = 0;
    struct adlak_device *padlak = (struct adlak_device *)data;

    if (padlak->hw_res.preg) {
        adlak_destroy_ioregion(padlak->hw_res.preg);
    }
    return ret;
}

int adlak_platform_set_clock(void *data, bool enable, int core_freq, int axi_freq)
{
	struct adlak_device *adla = data;
	struct clk *previous;
	bool was_enabled = adla->is_clk_core_enabled;
	int ret;

	if (!enable) {
		if (adla->is_clk_core_enabled)
			clk_disable_unprepare(adla->clk_core);
		adla->is_clk_core_enabled = false;
		adla->clk_core_freq_real = 0;
		return 0;
	}
	previous = clk_get_parent(adla->clk);
	ret = clk_set_parent(adla->clk, adla->clk_parent0);
	if (ret)
		return ret;
	ret = clk_set_rate(adla->clk_core, core_freq);
	if (ret)
		goto restore_parent;
	if (!was_enabled) {
		ret = clk_prepare_enable(adla->clk_core);
		if (ret)
			goto restore_parent;
	}
	adla->is_clk_core_enabled = true;
	adla->clk_core_freq_real = clk_get_rate(adla->clk_core);
	adla->clk_axi_freq_real = adla->clk_core_freq_real;
	adlak_dpm_clk_update(adla, core_freq, axi_freq);
	return 0;

restore_parent:
	clk_set_parent(adla->clk, previous);
	return ret;
}

int adlak_platform_resume(void *data)
{
	struct adlak_device *adla = data;
	int ret;

	if (!adla->is_suspend)
		return 0;
	ret = pm_runtime_resume_and_get(adla->dev);
	if (ret < 0)
		goto failed;
	ret = adlak_platform_set_clock(adla, true, adla->clk_core_freq_set,
				       adla->clk_axi_freq_set);
	if (ret) {
		pm_runtime_put_sync(adla->dev);
		goto failed;
	}
	adla->is_suspend = false;
	adla->power_error = 0;
	return 0;

failed:
	WRITE_ONCE(adla->power_error, ret);
	return ret;
}

void adlak_platform_suspend(void *data)
{
	struct adlak_device *adla = data;

	if (adla->is_suspend)
		return;
	adlak_hw_dev_suspend(adla);
	adlak_platform_set_clock(adla, false, 0, 0);
	adla->is_suspend = true;
	pm_runtime_mark_last_busy(adla->dev);
	pm_runtime_put_autosuspend(adla->dev);
}

int adlak_platform_pm_init(void *data)
{
	struct adlak_device *adla = data;
	int ret;

	adla->is_suspend = true;
	ret = adlak_os_sema_init(&adla->sem_pm_wakeup, 1, 0);
	if (ret)
		return ret;
	pm_runtime_set_autosuspend_delay(adla->dev, 300);
	pm_runtime_use_autosuspend(adla->dev);
	pm_runtime_enable(adla->dev);
	ret = adlak_platform_resume(adla);
	if (ret) {
		pm_runtime_disable(adla->dev);
		adlak_os_sema_destroy(&adla->sem_pm_wakeup);
	}
	return ret;
}

void adlak_platform_pm_deinit(void *data)
{
	struct adlak_device *adla = data;

	adlak_platform_suspend(adla);
	pm_runtime_barrier(adla->dev);
	pm_runtime_disable(adla->dev);
	pm_runtime_dont_use_autosuspend(adla->dev);
	adlak_os_sema_destroy(&adla->sem_pm_wakeup);
}
