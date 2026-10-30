// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2021 Amlogic, Inc. All rights reserved. */
#include <linux/regulator/consumer.h>

#include "adlak_regulator.h"
#include "adlak_os_base.h"
#include "adlak_device.h"

static void adlak_disable_supply(void *supply)
{
	regulator_disable(supply);
}

int adlak_voltage_init(void *data)
{
	struct adlak_device *adla = data;
	struct regulator *nn_regulator;
	int ret;

	nn_regulator = devm_regulator_get(adla->dev, "vddnpu");
	if (IS_ERR(nn_regulator))
		return PTR_ERR(nn_regulator);
	/* VIM4 baseline: nn_adj_vol=0, package=1, 890000 uV. */
	ret = regulator_set_voltage(nn_regulator, 890000, 890000);
	if (ret)
		return ret;
	ret = regulator_enable(nn_regulator);
	if (ret)
		return ret;
	return devm_add_action_or_reset(adla->dev, adlak_disable_supply,
				      nn_regulator);
}

int adlak_voltage_uninit(void *data)
{
	(void)data;
	return 0;
}
