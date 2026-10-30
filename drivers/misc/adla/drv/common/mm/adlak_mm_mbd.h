/*******************************************************************************
 * Copyright (C) 2024 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm_mbd.h
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

#ifndef __adlak_MM_MBP_H_13DAC1F72CD01573__
#define __adlak_MM_MBP_H_13DAC1F72CD01573__

/***************************** Include Files *********************************/
#include "adlak_mm.h"

#ifdef __cplusplus
extern "C" {
#endif

/************************** Constant Definitions *****************************/

/**************************Global Variable************************************/

/**************************Type Definition and Structure**********************/

/************************** Function Prototypes ******************************/

void adlak_mem_mbd_unregister(struct adlak_device *padlak, struct adlak_mem_operator *ops);

int adlak_mem_mbd_register(struct adlak_device *padlak, struct adlak_mem_operator *ops);

#ifdef __cplusplus
}
#endif

#endif /* __adlak_MM_MBP_H_13DAC1F72CD01573__ end define*/
