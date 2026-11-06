// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/mutex.h>
#include <media/meson-t7-dos.h>

#include "t7-dos-state.h"

static DEFINE_MUTEX(dos_lock);
static struct meson_t7_dos_state dos_state;

int meson_t7_dos_get(struct device *dev, bool exclusive)
{
	int ret;

	mutex_lock(&dos_lock);
	ret = meson_t7_dos_acquire(&dos_state, exclusive);
	mutex_unlock(&dos_lock);
	if (ret == -EBUSY)
		dev_dbg(dev, "DOS busy: decoder and encoders are mutually exclusive\n");
	else if (ret)
		dev_dbg(dev, "DOS poisoned until system reset\n");
	return ret;
}
EXPORT_SYMBOL_GPL(meson_t7_dos_get);

void meson_t7_dos_put(bool exclusive)
{
	mutex_lock(&dos_lock);
	meson_t7_dos_release(&dos_state, exclusive);
	mutex_unlock(&dos_lock);
}
EXPORT_SYMBOL_GPL(meson_t7_dos_put);

void meson_t7_dos_poison(void)
{
	mutex_lock(&dos_lock);
	if (!dos_state.poisoned) {
		dos_state.poisoned = true;
		__module_get(THIS_MODULE);
	}
	mutex_unlock(&dos_lock);
}
EXPORT_SYMBOL_GPL(meson_t7_dos_poison);

static void dos_put_shared(void *unused)
{
	meson_t7_dos_put(false);
}

int devm_meson_t7_dos_get_shared(struct device *dev)
{
	int ret = meson_t7_dos_get(dev, false);

	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, dos_put_shared, NULL);
}
EXPORT_SYMBOL_GPL(devm_meson_t7_dos_get_shared);

MODULE_DESCRIPTION("T7 DOS engine arbitration");
MODULE_LICENSE("GPL");
