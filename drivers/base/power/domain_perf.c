#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

LIST_HEAD(perfd_list);
DEFINE_MUTEX(perfd_list_lock);

static void perfd_lock_init(struct generic_perf_domain *perfd)
{
	if (perfd->flags & GENPD_FLAG_IRQ_SAFE) {
		spin_lock_init(&perfd->slock);
		perfd->irq_safe = true;
	} else {
		mutex_init(&perfd->mlock);
		perfd->irq_safe = false;
	}
}


static inline int perfd_lock_irq(struct generic_perf_domain *perfd)
	__acquires(&perfd->mlock)
{
	mutex_lock(&perfd->mlock);
	return 0;
}


static inline void perfd_unlock_irq(struct generic_perf_domain *perfd)
	__releases(&perfd->mlock)
{
	mutex_unlock(&perfd->mlock);
}


static inline int perfd_lock_noirq(struct generic_perf_domain *perfd)
	__acquires(&perfd->slock)
{
	unsigned long flags;


	spin_lock_irqsave(&perfd->slock, flags);
	return 0;
}

static inline void perfd_unlock_noirq(struct generic_perf_domain *perfd)
	__releases(&perfd->slock)
{
	spin_unlock_irqrestore(&perfd->slock, perfd->lock_flags);
}

int perfd_lock(struct generic_perf_domain *perfd)
{
	return perfd->irq_safe ? perfd_lock_noirq(perfd)
		: perfd_lock_irq(perfd);
}

void perfd_unlock(struct generic_perf_domain *perfd)
{
	return perfd->irq_safe ? perfd_unlock_noirq(perfd)
		: perfd_unlock_irq(perfd);
}

bool perfd_can_scale(struct generic_perf_domain *perfd)
{
	struct generic_pm_domain *genpd;
	struct pm_domain_data *pdd;

	list_for_each_entry(genpd, &perfd->power_domain_list, perfd_node) {
		list_for_each_entry(pdd, &genpd->dev_list, list_node) {
			if (!pm_runtime_scaling_allowed(pdd->dev))
				return false;
		}
	}

	return true;
}

int perfd_dev_set_pstate_default(struct device *dev, unsigned int state)
{
	int (*cb)(struct device *__dev, unsigned int state);

	if (dev->type && dev->type->pm)
		cb = dev->type->pm->pstate_set;
	else if (dev->class && dev->class->pm)
		cb = dev->class->pm->pstate_set;
	else if (dev->bus && dev->bus->pm)
		cb = dev->bus->pm->pstate_set;
	else
		cb = NULL;

	if (!cb && dev->driver && dev->driver->pm)
		cb = dev->driver->pm->pstate_set;

	return cb ? cb(dev, state) : 0;
}

int perfd_dev_set_pstate(struct device *dev, unsigned int state)
{
	struct generic_pm_domain *genpd = NULL;
	struct generic_perf_domain *perfd = NULL;
	struct generic_pm_domain_data *gpd_data = NULL;
	int ret = -EINVAL;

	if (!dev)
		return ret;

	if (dev->power.subsys_data && dev->power.subsys_data->domain_data)
		gpd_data = dev_gpd_data(dev);

	if (!gpd_data) {
		pr_err("No domain data for device %s\n", dev_name(dev));
		return ret;
	}

	genpd = dev_to_genpd(dev);
	if (!genpd) {
		pr_err("No domain for device %s\n", dev_name(dev));
		return ret;
	}

	perfd = genpd->perfd;
	if (!perfd) {
		pr_err("No perf domain for device %s\n", dev_name(dev));
		return ret;
	}

	if (gpd_data->sd.pstate == state)
		return 0;

	ret = perfd_lock(perfd);
	if (ret)
		return ret;

	if (!perfd_can_scale(perfd)) {
		perfd_unlock(perfd);
		return -EBUSY;
	}

	gpd_data->sd.requested_pstate = state;

	ret = perfd->gov->update(perfd);
	if (ret)
		pr_warn("Governor could not update");

	perfd_unlock(perfd);

	return 0;
}

int perfd_add_genpd(struct generic_perf_domain *perfd,
			struct generic_pm_domain *genpd,
			unsigned int flags)
{
	if (IS_ERR_OR_NULL(genpd) || IS_ERR_OR_NULL(perfd))
		return -EINVAL;

	if (genpd->irq_safe != perfd->irq_safe) {
		pr_err("Genpd does not match perfd irq_safe flag.\n");
		return -EINVAL;
	}

	perfd_lock(perfd);

	if (flags & PERFD_FLAGS_GENPD_IS_ROOT) {
		if (perfd->root) {
			pr_err("Can not have 2 root nodes on perfdomain\n");
			perfd_unlock(perfd);
			return -EINVAL;
		}
		perfd->root = genpd;
	}

	list_add(&genpd->perfd_node, &perfd->power_domain_list);

	genpd->domain.ops.pstate_set = perfd_dev_set_pstate;
	genpd->perfd = perfd;

	perfd_unlock(perfd);

	return 0;
}

int perf_domain_init(struct generic_perf_domain *perfd,
			struct generic_scale_governor *gov)
{
	if (IS_ERR_OR_NULL(perfd))
		return -EINVAL;

	if (gov == NULL)
		perfd->gov = &scale_gov_simple;
	else
		perfd->gov = gov;

	INIT_LIST_HEAD(&perfd->power_domain_list);

	perfd_lock_init(perfd);
	mutex_lock(&perfd_list_lock);
	list_add(&perfd->node, &perfd_list);
	mutex_unlock(&perfd_list_lock);

	return 0;
}
