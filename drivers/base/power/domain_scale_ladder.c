#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#define SCALE_UP 0
#define SCALE_DOWN 1
#define NO_SCALE 2

int perfd_choose_state_bottom_up(struct generic_pm_domain *genpd)
{
	struct gpd_link *link;
	unsigned int ret;

	if (!genpd->pending_update)
		return 0;

	list_for_each_entry(link, &genpd->master_links, master_node) {
		if (link->slave->pending_update) {
			ret = perfd_choose_state_bottom_up(link->slave);
			if (ret)
				return ret;
		}
	}

	if (genpd->get_next_state) {
		ret = genpd->get_next_state(genpd, &genpd->next_state);
		if (ret)
			return ret;
	}

	genpd->pending_update = false;

	list_for_each_entry(link, &genpd->slave_links, slave_node) {
		if (link->master->pending_update) {
			ret = perfd_choose_state_bottom_up(link->slave);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int perfd_set_state(struct generic_pm_domain *genpd)
{
	int ret;

	if (genpd->next_state != genpd->state) {
		if (genpd->set_state) {
			ret = genpd->set_state(genpd, genpd->next_state);
			if (ret)
				return ret;
		}
		genpd->old_state = genpd->state;
		genpd->state = genpd->next_state;
	}

	return 0;
}

int perfd_set_state_top_down(struct generic_pm_domain *genpd)
{
	struct gpd_link *link;
	int ret;

	if (!genpd->pending_update)
		return 0;

	list_for_each_entry(link, &genpd->slave_links, slave_node) {
		if (link->master->pending_update)
			ret = perfd_set_state_top_down(link->slave);
			if (ret)
				return ret;
	}

	ret = perfd_set_state(genpd);
	if (ret)
		return ret;

	genpd->pending_update = false;

	list_for_each_entry(link, &genpd->master_links, master_node) {
		if (link->slave->pending_update)
			ret = perfd_set_state_top_down(link->slave);
			if (ret)
				return ret;
	}

	return 0;
}

int perfd_set_state_bottom_up(struct generic_pm_domain *genpd)
{
	struct gpd_link *link;
	int ret;

	if (!genpd->pending_update)
		return 0;

	list_for_each_entry(link, &genpd->master_links, master_node) {
		if (link->slave->pending_update)
			ret = perfd_set_state_bottom_up(link->slave);
			if (ret)
				return ret;
	}

	ret = perfd_set_state(genpd);
	if (ret)
		return ret;

	genpd->pending_update = false;

	list_for_each_entry(link, &genpd->slave_links, slave_node) {
		if (link->master->pending_update)
			ret = perfd_set_state_bottom_up(link->slave);
			if (ret)
				return ret;
	}

	return 0;
}


int perfd_mark_all_pending(struct generic_perf_domain *perfd)
{
	struct generic_pm_domain *genpd;

	list_for_each_entry(genpd, &perfd->power_domain_list, perfd_node)
		genpd->pending_update = true;

	return 0;
}

int perf_get_scale_direction(struct generic_perf_domain *perfd)
{
	struct generic_pm_domain *genpd;

	/*there should only be one device that has changed.?*/
	list_for_each_entry(genpd, &perfd->power_domain_list, perfd_node) {
		if (genpd->next_state > genpd->state)
			return SCALE_UP;
		else if (genpd->next_state < genpd->state)
			return SCALE_DOWN;
	}
	return NO_SCALE;
}

int perfd_update(struct generic_perf_domain *perfd)
{
	unsigned int scale_dir;
	int ret;

	ret = perfd_mark_all_pending(perfd);
	if (ret)
		return ret;

	ret = perfd_choose_state_bottom_up(perfd->root);
	if (ret)
		return ret;

	scale_dir = perf_get_scale_direction(perfd);

	if (scale_dir == NO_SCALE)
		return 0;

	ret = perfd_mark_all_pending(perfd);
	if (ret)
		return ret;

	if (scale_dir == SCALE_UP) {
		ret = perfd_set_state_top_down(perfd->root);
		if (ret)
			return ret;

	} else {
		ret = perfd_set_state_bottom_up(perfd->root);
		if (ret)
			return ret;
	}

	return 0;
}

struct generic_scale_governor scale_gov_ladder = {
	.update = perfd_update,
};
