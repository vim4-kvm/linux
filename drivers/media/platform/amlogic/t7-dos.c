// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/mutex.h>
#include <media/meson-t7-dos.h>

static DEFINE_MUTEX(dos_lock);
static unsigned int encoders;
static bool decoder;
static bool poisoned;

int meson_t7_dos_get(struct device *dev, bool exclusive)
{
	int ret = 0;

	mutex_lock(&dos_lock);
	if (poisoned)
		ret = -EIO;
	else if (decoder || (exclusive && encoders))
		ret = -EBUSY;
	else if (exclusive)
		decoder = true;
	else
		encoders++;
	mutex_unlock(&dos_lock);
	if (ret)
		dev_err(dev, "T7 VDEC requires Wave and JPEG drivers unloaded\n");
	return ret;
}
EXPORT_SYMBOL_GPL(meson_t7_dos_get);

void meson_t7_dos_put(bool exclusive)
{
	mutex_lock(&dos_lock);
	if (exclusive)
		decoder = false;
	else if (encoders)
		encoders--;
	mutex_unlock(&dos_lock);
}
EXPORT_SYMBOL_GPL(meson_t7_dos_put);

void meson_t7_dos_poison(void)
{
	mutex_lock(&dos_lock);
	if (!poisoned) {
		poisoned = true;
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
