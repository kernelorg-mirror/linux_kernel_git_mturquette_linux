#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

int perfd_dev_set_pstate_all(struct generic_pm_domain *genpd,
		unsigned int state)
{
	struct pm_domain_data *pdd;
	int ret;

	list_for_each_entry(pdd, &genpd->dev_list, list_node) {
		struct generic_pm_domain_data *gpd_data;

		gpd_data = dev_gpd_data(pdd->dev);
		if (gpd_data->sd.pstate != state) {
			ret = perfd_dev_set_pstate_default(pdd->dev, state);
			if (ret) {
				pr_warn("Could not set state for device.\n");
				continue;
			}
			gpd_data->sd.pstate = state;
		}
	}

	return 0;
}

int perfd_update_simple(struct generic_perf_domain *perfd)
{
	struct generic_pm_domain_data *gpd_data;
	struct generic_pm_domain *genpd;
	struct pm_domain_data *pdd;

	unsigned int biggest_pstate = 0;
	int ret;

	list_for_each_entry(genpd, &perfd->power_domain_list, perfd_node) {
		list_for_each_entry(pdd, &genpd->dev_list, list_node) {
			gpd_data = dev_gpd_data(pdd->dev);
			if (gpd_data->sd.requested_pstate > biggest_pstate)
				biggest_pstate = gpd_data->sd.requested_pstate;
		}
	}


	list_for_each_entry(genpd, &perfd->power_domain_list, perfd_node) {
		if (genpd->set_state && (biggest_pstate != genpd->state)) {
			ret = genpd->set_state(genpd, biggest_pstate);
			if (ret)
				return ret;
		}

		perfd_dev_set_pstate_all(genpd, biggest_pstate);
	}

	return 0;
}

struct generic_scale_governor scale_gov_simple = {
	.update = perfd_update_simple,
};
