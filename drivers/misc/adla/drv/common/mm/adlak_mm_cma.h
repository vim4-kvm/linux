/*******************************************************************************
 * Copyright (C) 2024 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_mm_cma.h
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

#ifndef __adlak_MM_CMA_H_1DA668207C41A574__
#define __adlak_MM_CMA_H_1DA668207C41A574__

/***************************** Include Files *********************************/
#include "adlak_mm.h"

#ifdef __cplusplus
extern "C" {
#endif

/************************** Constant Definitions *****************************/

/**************************Global Variable************************************/

/**************************Type Definition and Structure**********************/

/************************** Function Prototypes ******************************/

void adlak_mem_cma_unregister(struct adlak_device *padlak, struct adlak_mem_operator *ops);

int adlak_mem_cma_register(struct adlak_device *padlak, struct adlak_mem_operator *ops);

#ifdef __cplusplus
}
#endif

#endif /* __adlak_MM_CMA_H_1DA668207C41A574__ end define*/
