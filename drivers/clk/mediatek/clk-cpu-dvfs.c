/*
 * Copyright (c) 2015 Linaro Ltd.
 * Author: Pi-Cheng Chen <pi-cheng.chen@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "clk-mtk.h"

#define MIN_VOLT_SHIFT		(100000)
#define MAX_VOLT_SHIFT		(200000)
#define MAX_VOLT_LIMIT		(1150000)
#define VOLT_TOL		(10000)

struct mtk_cpu_dvfs_info {
	struct device *cpu_dev;
	struct regulator *proc_reg;
	struct regulator *sram_reg;
	struct clk *inter_clk;
	int intermediate_voltage;
	bool need_voltage_tracking;
};

static int cpu_dvfs_voltage_tracking(struct mtk_cpu_dvfs_info *info,
				     int new_vproc)
{
	struct regulator *proc_reg = info->proc_reg;
	struct regulator *sram_reg = info->sram_reg;
	int old_vproc, old_vsram, new_vsram, vsram, vproc, ret;

	old_vproc = regulator_get_voltage(proc_reg);
	if (old_vproc < 0) {
		dev_err(info->cpu_dev, "invalid Vproc voltage!(%d)\n",
			old_vproc);
		return old_vproc;
	}

	/* Vsram should not exceed the maximum allowed voltage of SoC. */
	new_vsram = min(new_vproc + MIN_VOLT_SHIFT, MAX_VOLT_LIMIT);

	if (old_vproc < new_vproc) {
		/*
		 * When scaling up voltages, Vsram and Vproc scale up step
		 * by step. At each step, set Vsram to (Vproc + 200mV) first,
		 * then set Vproc to (Vsram - 100mV).
		 * Keep doing it until Vsram and Vproc hit target voltages.
		 */
		do {
			old_vsram = regulator_get_voltage(sram_reg);
			if (old_vsram < 0) {
				dev_err(info->cpu_dev,
					"invalid Vsram voltage!(%d)\n",
					old_vsram);
				return old_vsram;
			}

			old_vproc = regulator_get_voltage(proc_reg);
			if (old_vproc < 0) {
				dev_err(info->cpu_dev,
					"invalid Vproc voltage(%d)\n",
					old_vproc);
				return old_vproc;
			}

			vsram = min(new_vsram, old_vproc + MAX_VOLT_SHIFT);

			if (vsram + VOLT_TOL >= MAX_VOLT_LIMIT) {
				vsram = MAX_VOLT_LIMIT;

				/*
				 * If the target Vsram hits the maximum voltage,
				 * try to set the exact voltage value first.
				 */
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram);
				if (ret)
					ret = regulator_set_voltage(sram_reg,
							vsram - VOLT_TOL,
							vsram);

				vproc = new_vproc;
			} else {
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram + VOLT_TOL);

				vproc = vsram - MIN_VOLT_SHIFT;
			}
			if (ret)
				return ret;

			ret = regulator_set_voltage(proc_reg, vproc,
						    vproc + VOLT_TOL);
			if (ret) {
				regulator_set_voltage(sram_reg, old_vsram,
						      old_vsram);
				return ret;
			}
		} while (vproc < new_vproc || vsram < new_vsram);
	} else if (old_vproc > new_vproc) {
		/*
		 * When scaling down voltages, Vsram and Vproc scale down step
		 * by step. At each step, set Vproc to (Vsram - 200mV) first,
		 * then set Vproc to (Vproc + 100mV).
		 * Keep doing it until Vsram and Vproc hit target voltages.
		 */
		do {
			old_vproc = regulator_get_voltage(proc_reg);
			if (old_vproc < 0) {
				dev_err(info->cpu_dev,
					"invalid Vproc voltage!(%d)\n",
					old_vproc);
				return old_vproc;
			}

			old_vsram = regulator_get_voltage(sram_reg);
			if (old_vsram < 0) {
				dev_err(info->cpu_dev,
					"invalid Vsram voltage!(%d)\n",
					old_vsram);
				return old_vsram;
			}

			vproc = max(new_vproc, old_vsram - MAX_VOLT_SHIFT);
			ret = regulator_set_voltage(proc_reg, vproc,
						    vproc + VOLT_TOL);
			if (ret)
				return ret;

			if (vproc == new_vproc)
				vsram = new_vsram;
			else
				vsram = max(new_vsram, vproc + MIN_VOLT_SHIFT);

			if (vsram + VOLT_TOL >= MAX_VOLT_LIMIT) {
				vsram = MAX_VOLT_LIMIT;

				/*
				 * If the target Vsram hits the maximum voltage,
				 * try to set the exact voltage value first.
				 */
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram);
				if (ret)
					ret = regulator_set_voltage(sram_reg,
							vsram - VOLT_TOL,
							vsram);
			} else {
				ret = regulator_set_voltage(sram_reg, vsram,
							    vsram + VOLT_TOL);
			}

			if (ret) {
				regulator_set_voltage(proc_reg, old_vproc,
						      old_vproc);
				return ret;
			}
		} while (vproc > new_vproc + VOLT_TOL ||
			 vsram > new_vsram + VOLT_TOL);
	}

	return 0;
}

static int cpu_dvfs_set_voltage(struct mtk_cpu_dvfs_info *info, int vproc)
{
	if (info->need_voltage_tracking)
		return cpu_dvfs_voltage_tracking(info, vproc);
	else
		return regulator_set_voltage(info->proc_reg, vproc,
					     vproc + VOLT_TOL);
}

int cpu_dvfs_coordinate_rates(const struct coord_rate_domain *crd,
			      int rate_idx)
{
	struct clk_hw *mux_hw, *pll_hw;
	struct mtk_cpu_dvfs_info *info = crd->priv;
	struct dev_pm_opp *opp;
	int vproc, old_vproc, inter_vproc, target_vproc, ret;
	unsigned long pll_parent_rate, rate, old_rate;

	mux_hw = crd->table[CPU_DVFS_MUX_INDEX][rate_idx].hw;
	pll_hw = crd->table[CPU_DVFS_PLL_INDEX][rate_idx].hw;
	rate = crd->table[CPU_DVFS_PLL_INDEX][rate_idx].rate;
	pll_parent_rate = crd->table[CPU_DVFS_PLL_INDEX][rate_idx].parent_rate;

	inter_vproc = info->intermediate_voltage;

	old_rate = clk_hw_get_rate(pll_hw);
	old_vproc = regulator_get_voltage(info->proc_reg);
	if (old_vproc < 0) {
		dev_err(info->cpu_dev,
			"invalid voltage value for Vproc!(%d)\n", old_vproc);
		return old_vproc;
	}

	rcu_read_lock();
	opp = dev_pm_opp_find_freq_ceil(info->cpu_dev, &rate);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		dev_err(info->cpu_dev,
			"failed to find OPP for %ld\n", rate);
		return PTR_ERR(opp);
	}
	vproc = dev_pm_opp_get_voltage(opp);
	rcu_read_unlock();

#define ARM_PLL		1
#define MAIN_PLL	2

	/*
	 * If the new voltage or the intermediate voltage is higher than the
	 * current voltage, scale up voltage first.
	 */
	target_vproc = (inter_vproc > vproc) ? inter_vproc : vproc;
	if (old_vproc < target_vproc) {
		ret = cpu_dvfs_set_voltage(info, target_vproc);
		if (ret) {
			dev_err(info->cpu_dev, "failed to scale up voltage!\n");
			cpu_dvfs_set_voltage(info, old_vproc);
			return ret;
		}
	}

	/* Reparent the CPU clock to intermediate clock. */
	ret = clk_mux_set_parent(mux_hw, MAIN_PLL);
	if (ret) {
		dev_err(info->cpu_dev,
			"failed to switch MUX to intermediate clock\n");
		cpu_dvfs_set_voltage(info, old_vproc);
		WARN_ON(1);
		return ret;
	}

	/* Set the original PLL to target rate. */
	ret = mtk_pll_set_rate(pll_hw, rate, pll_parent_rate);
	if (ret) {
		dev_err(info->cpu_dev, "failed to scale up PLL rate\n");
		clk_mux_set_parent(mux_hw, ARM_PLL);
		cpu_dvfs_set_voltage(info, old_vproc);
	}

	/* Set parent of CPU clock back to the original PLL. */
	ret = clk_mux_set_parent(mux_hw, ARM_PLL);
	if (ret) {
		dev_err(info->cpu_dev, "failed to switch mux to ARM PLL\n");
		cpu_dvfs_set_voltage(info, inter_vproc);
		WARN_ON(1);
		return ret;
	}

	/*
	 * If the new voltage is lower than the intermediate voltage or the
	 * original voltage, scale down to the new voltage.
	 */
	if (vproc < inter_vproc || vproc < old_vproc) {
		ret = cpu_dvfs_set_voltage(info, vproc);
		if (ret) {
			dev_err(info->cpu_dev,
				"failed to scale down voltage!\n");
			clk_mux_set_parent(mux_hw, MAIN_PLL);
			mtk_pll_set_rate(pll_hw, old_rate, pll_parent_rate);
			clk_mux_set_parent(mux_hw, ARM_PLL);
			return ret;
		}
	}

	return 0;
}

static int mtk_cpu_dvfs_info_init(int cpu, struct mtk_cpu_dvfs_info **cpu_dvfs_info)
{
	struct device *cpu_dev;
	struct regulator *proc_reg = ERR_PTR(-ENODEV);
	struct regulator *sram_reg = ERR_PTR(-ENODEV);
	struct clk *inter_clk = ERR_PTR(-ENODEV);
	struct dev_pm_opp *opp;
	struct mtk_cpu_dvfs_info *info;
	unsigned long rate;
	int ret;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev) {
		pr_err("failed to get cpu%d device\n", cpu);
		ret = -ENODEV;
		goto out_free_cpu_dvfs_info;
	}

	inter_clk = clk_get(cpu_dev, "intermediate");
	if (IS_ERR(inter_clk)) {
		pr_err("failed to get intermediate clk for cpu%d\n", cpu);
		ret = PTR_ERR(inter_clk);
		goto out_free_cpu_dvfs_info;
	}

	proc_reg = regulator_get_exclusive(cpu_dev, "proc");
	if (IS_ERR(proc_reg)) {
		pr_err("failed to get proc regulator for cpu%d\n", cpu);
		ret = PTR_ERR(proc_reg);
		goto out_release_resources;
	}

	/* Both presence and absence of sram regulator are valid cases. */
	sram_reg = regulator_get_exclusive(cpu_dev, "sram");

	ret = of_init_opp_table(cpu_dev);
	if (ret) {
		pr_err("failed to init opp table for cpu%d\n", cpu);
		goto out_release_resources;
	}

	/* Search a safe voltage for intermediate frequency. */
	rate = clk_get_rate(inter_clk);
	rcu_read_lock();
	opp = dev_pm_opp_find_freq_ceil(cpu_dev, &rate);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		pr_err("failed to get intermediate opp for cpu%d\n", cpu);
		ret = PTR_ERR(opp);
		goto out_free_opp_table;
	}
	info->intermediate_voltage = dev_pm_opp_get_voltage(opp);
	rcu_read_unlock();

	of_free_opp_table(cpu_dev);

	info->cpu_dev = cpu_dev;
	info->proc_reg = proc_reg;
	info->sram_reg = IS_ERR(sram_reg) ? NULL : sram_reg;
	info->inter_clk = inter_clk;

	/*
	 * If SRAM regulator is present, software "voltage tracking" is needed
	 * for this CPU power domain.
	 */
	info->need_voltage_tracking = !IS_ERR(sram_reg);

	*cpu_dvfs_info = info;

	return 0;

out_free_opp_table:
	of_free_opp_table(cpu_dev);
out_release_resources:
	if (!IS_ERR(info->proc_reg))
		regulator_put(info->proc_reg);
	if (!IS_ERR(info->sram_reg))
		regulator_put(info->sram_reg);
	if (!IS_ERR(info->inter_clk))
		clk_put(info->inter_clk);
out_free_cpu_dvfs_info:
	kfree(info);

	return ret;
}

void mtk_cpu_dvfs_domain_release(struct coord_rate_domain *domain)
{
	struct mtk_cpu_dvfs_info *info = domain->priv;

	if (!info)
		return;

	if (!IS_ERR(info->proc_reg))
		regulator_put(info->proc_reg);
	if (!IS_ERR(info->sram_reg))
		regulator_put(info->sram_reg);
	if (!IS_ERR(info->inter_clk))
		clk_put(info->inter_clk);

	kfree(info);
}

int mtk_cpu_dvfs_domain_init(struct coord_rate_domain *domain, int cpu)
{
	struct mtk_cpu_dvfs_info *info = NULL;
	int ret;

	ret = mtk_cpu_dvfs_info_init(cpu, &info);
	if (ret)
		pr_err("Failed to initialize CPU DVFS domain for cpu%d\n", cpu);

	domain->priv = info;

	return ret;
}
