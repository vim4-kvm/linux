/*******************************************************************************
 * Copyright (C) 2021 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_platform_addon.c
 * @brief
 *
 * <pre>
 * MODIFICATION HISTORY:
 *
 * Ver   	Who				Date				Changes
 * ----------------------------------------------------------------------------
 * 1.00a sh nn team 2024/04/09	Initial release
 * </pre>
 *
 ******************************************************************************/

/***************************** Include Files *********************************/
/* adjust nn voltage */
#include <linux/arm-smccc.h>
#include <linux/amlogic/media/registers/cpu_version.h>
#include <linux/regulator/consumer.h>

#include "adlak_platform_addon.h"
#include "adlak_os_base.h"
#include "adlak_device.h"

static long adlak_axi_sram_setup(unsigned long function_id, unsigned long arg0) {
    struct arm_smccc_res res;

    arm_smccc_smc(function_id, arg0, 0, 0, 0, 0, 0, 0, &res);
    return res.a0;
}

int adlak_axi_sram_init(void *data) {
    struct adlak_device *padlak = (struct adlak_device *)data;
    u32 adlak_smc_cmd = 0;
    int ret = -1;

#ifdef CONFIG_OF
    of_property_read_u32(padlak->dev->of_node, "adla_smc_cmd", &adlak_smc_cmd);
#endif

    printk("cc: adlak_smc_cmd: 0x%lx\n", adlak_smc_cmd);
    if (adlak_smc_cmd != 0) {
        ret = adlak_axi_sram_setup((unsigned long)adlak_smc_cmd, 0);
        if (ret != 0) {
            return -1;
        }
    }
    return 0;
}
