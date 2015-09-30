#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/device.h>

extern struct dev_pm_opp *dev_pm_opp_get_opp(struct device *dev,
					     unsigned int index);
struct opp_scale_device {
	struct list_head node;
	struct device *dev;
	struct regulator *reg;
	struct clk *clk;
	unsigned int cur_opp_idx;
	/* lock? */
};

LIST_HEAD(scale_devices);
DEFINE_MUTEX(opp_scale_list_lock);

struct opp_scale_device *find_opp_scale_device(struct device *dev)
{
	struct opp_scale_device *scale_dev;

	list_for_each_entry(scale_dev, &scale_devices, node) {
		if (scale_dev->dev == dev)
			return scale_dev;
	}

	return NULL;
}

int opp_scale(struct device *dev, unsigned int new_opp_idx)
{
	struct opp_scale_device *scale_dev;
	struct dev_pm_opp *opp;
	unsigned long opp_freq;
	unsigned long opp_volt;
	unsigned long tol = 0;
	bool scale_up = false;
	int ret;

	mutex_lock(&opp_scale_list_lock);

	scale_dev = find_opp_scale_device(dev);
	if (!scale_dev) {
		mutex_unlock(&opp_scale_list_lock);
		return -EEXIST;
	}

	mutex_unlock(&opp_scale_list_lock);

	if (new_opp_idx == scale_dev->cur_opp_idx)
		return 0;

	rcu_read_lock();
	opp = dev_pm_opp_get_opp(dev, new_opp_idx);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		return -EINVAL;
	}

	opp_volt = dev_pm_opp_get_voltage(opp);
	opp_freq = dev_pm_opp_get_freq(opp);
	rcu_read_unlock();

	if (new_opp_idx > scale_dev->cur_opp_idx)
		scale_up = true;

	if (scale_dev->reg && scale_up) {
		ret = regulator_set_voltage_tol(scale_dev->reg, opp_volt, tol);
		if (ret) {
			pr_err("Error setting reg voltage\n");
			return ret;
		}
	}

	if (scale_dev->clk) {
		ret = clk_set_rate(scale_dev->clk, opp_freq);
		if (ret) {
			pr_err("Error setting clk rate\n");
			goto err;
		}
	}

	if (scale_dev->reg && !scale_up) {
		ret = regulator_set_voltage_tol(scale_dev->reg, opp_volt, tol);
		if (ret) {
			pr_err("Error setting reg voltage\n");
			goto err;
		}
	}

	scale_dev->cur_opp_idx = new_opp_idx;
	return 0;
err:
	if (scale_dev->reg) {
		rcu_read_lock();
		opp = dev_pm_opp_get_opp(dev, scale_dev->cur_opp_idx);
		if (IS_ERR(opp)) {
			rcu_read_unlock();
			return -EINVAL;
		}
		opp_volt = dev_pm_opp_get_voltage(opp);
		opp_freq = dev_pm_opp_get_freq(opp);
		rcu_read_unlock();

		if (scale_up)
			regulator_set_voltage_tol(scale_dev->reg,
							opp_volt, tol);
		else
			clk_set_rate(scale_dev->clk, opp_freq);

	}

	return ret;
}

int opp_scale_register(struct device *dev, char *reg_name, char *clk_name)
{
	struct opp_scale_device *scale_dev;
	struct regulator *reg;
	struct clk *clk;
	int ret = 0;

	if (!dev)
		return -EINVAL;

	mutex_lock(&opp_scale_list_lock);

	scale_dev = find_opp_scale_device(dev);
	if (scale_dev) {
		ret = -EEXIST;
		goto out_unlock;
	}

	scale_dev =  kzalloc(sizeof(*scale_dev), GFP_KERNEL);
	if (!scale_dev) {
		ret = -ENOMEM;
		goto out_unlock;
	}


	scale_dev->dev = dev;

	reg = regulator_get_optional(dev, reg_name);
	if (IS_ERR(reg)) {
		if (PTR_ERR(reg) == -EPROBE_DEFER) {
			dev_dbg(dev, "regulator not ready.\n");
			ret = -EPROBE_DEFER;
			goto out_free_opp;
		}
		dev_dbg(dev, "%s: no regulator.\n", dev_name(dev));
	} else {
		scale_dev->reg = reg;
	}

	clk = clk_get(dev, clk_name);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		if (ret == -EPROBE_DEFER)
			dev_dbg(dev, "clock not ready, retry\n");
		else
			dev_err(dev, "failed to get clock:\n");

		goto out_free_opp;
	}

	scale_dev->clk = clk;

	ret = of_init_opp_table(dev);
	if (ret)
		goto out_free_opp;

	list_add_tail(&scale_dev->node, &scale_devices);

	mutex_unlock(&opp_scale_list_lock);
	return 0;

out_free_opp:
	if (scale_dev->reg)
		regulator_put(scale_dev->reg);
	if (scale_dev->clk)
		clk_put(scale_dev->clk);

	of_free_opp_table(dev);

	kfree(scale_dev);

out_unlock:
	mutex_unlock(&opp_scale_list_lock);

	return ret;
}

int opp_scale_unregister(struct device *dev)
{
	struct opp_scale_device *scale_dev;

	if (!dev)
		return -EINVAL;

	mutex_lock(&opp_scale_list_lock);
	scale_dev = find_opp_scale_device(dev);
	if (!scale_dev) {
		mutex_unlock(&opp_scale_list_lock);
		return -EEXIST;
	}

	list_del(&scale_dev->node);
	mutex_unlock(&opp_scale_list_lock);

	if (scale_dev->reg)
		regulator_put(scale_dev->reg);

	if (scale_dev->clk)
		clk_put(scale_dev->clk);

	of_free_opp_table(dev);

	kfree(scale_dev);

	return 0;
}
