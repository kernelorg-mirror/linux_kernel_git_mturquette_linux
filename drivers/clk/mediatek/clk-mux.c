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

#include <linux/mfd/syscon.h>
#include <linux/slab.h>

#include "clk-mtk.h"

struct mtk_clk_mux {
	struct clk_hw	hw;
	struct regmap	*regmap;
	u32		reg;
	u32		mask;
	u8		shift;
};

inline struct mtk_clk_mux *to_clk_mux(struct clk_hw *hw)
{
	return container_of(hw, struct mtk_clk_mux, hw);
}

static u8 clk_mux_get_parent(struct clk_hw *hw)
{
	struct mtk_clk_mux *mux = to_clk_mux(hw);
	int num_parents = clk_hw_get_num_parents(hw);
	unsigned int val;

	regmap_read(mux->regmap, mux->reg, &val);

	val >>= mux->shift;
	val &= mux->mask;

	if (val >= num_parents)
		return -EINVAL;

	return val;
}

static const struct clk_ops cpu_dvfs_mux_ops = {
	.get_parent = clk_mux_get_parent,
};

static struct clk __init *mtk_clk_register_mux(const struct mtk_composite *data,
			struct regmap *regmap)
{
	struct mtk_clk_mux *mux;
	struct clk *clk;
	struct clk_init_data init;
	int i;

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return ERR_PTR(-ENOMEM);

	init.name = data->name;
	init.ops = &cpu_dvfs_mux_ops;
	init.parent_names = data->parent_names;
	init.num_parents = data->num_parents;
	init.flags = data->flags;

	mux->reg = data->mux_reg;
	mux->shift = data->mux_shift;
	mux->mask = BIT(data->mux_width) - 1;
	mux->regmap = regmap;
	mux->hw.init = &init;

	clk = clk_register(NULL, &mux->hw);
	if (IS_ERR(clk))
		kfree(mux);

	return clk;
}

int __init mtk_clk_register_muxes(struct device_node *node,
			const struct mtk_composite *clks, int num,
			struct clk_onecell_data *clk_data)
{
	int i;
	struct clk *clk;
	struct regmap *regmap;

	regmap = syscon_node_to_regmap(node);
	if (IS_ERR(regmap)) {
		pr_err("Cannot find regmap for %s: %ld\n", node->full_name,
		       PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	for (i = 0; i < num; i++) {
		const struct mtk_composite *mux = &clks[i];

		clk = mtk_clk_register_mux(mux, regmap);
		if (IS_ERR(clk)) {
			pr_err("Failed to regiter clk %s: %ld\n",
			       mux->name, PTR_ERR(clk));
			continue;
		}

		clk_data->clks[mux->id] = clk;
	}

	return 0;
}
