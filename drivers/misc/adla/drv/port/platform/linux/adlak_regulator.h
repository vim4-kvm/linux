/*******************************************************************************
 * Copyright (C) 2021 Amlogic, Inc. All rights reserved.
 ******************************************************************************/

/*****************************************************************************/
/**
 *
 * @file adlak_regulator.h
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
#include <linux/module.h>
#include <linux/amlogic/gki_module.h>

int adlak_voltage_init(void *data);
int adlak_voltage_uninit(void *data);
