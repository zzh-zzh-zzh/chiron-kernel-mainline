/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2019 Linaro Limited
 * Copyright (c) 2021, AngeloGioacchino Del Regno
 *                     <angelogioacchino.delregno@somainline.org>
 */

#ifndef __CPR_H__
#define __CPR_H__

struct cpr_ext_data {
	int mem_acc_threshold_uV;
	int apm_threshold_uV;
};

struct device;
int chiron_cprh_init_cpu(struct device *pd_dev, struct device *cpu_dev);
void chiron_cprh_release_cpu(struct device *pd_dev, struct device *cpu_dev);

#endif /* __CPR_H__ */
