// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Inochi Amaoto <inochiama@outlook.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <soc/sophgo/cv1800-sysctl.h>

#define PHY_IDPAD_C_OW			BIT(6)
#define PHY_IDPAD_C_SW			BIT(7)

#define PHY_BASE_CLK_RATE		300000000
#define PHY_APP_CLK_RATE		125000000
#define PHY_LPM_CLK_RATE		12000000
#define PHY_STB_CLK_RATE		333334

#define PHY_VBUS_DET_DEBOUNCE_TIME	usecs_to_jiffies(100)

struct cv1800_usb_phy {
	struct phy *phy;
	struct regmap *syscon;
	spinlock_t lock;
	struct clk *usb_phy_clk;
	struct clk *usb_app_clk;
	struct clk *usb_lpm_clk;
	struct clk *usb_stb_clk;
	struct gpio_descs *switch_gpios;
	struct gpio_desc *vbus_det_gpio;
	int vbus_det_irq;
	struct delayed_work vbus_work;
	bool enable_otg;
};

static int cv1800_usb_phy_set_idpad(struct cv1800_usb_phy *phy,
				    bool is_host)
{
	unsigned long flags;
	unsigned int regval = 0;
	int ret;

	if (is_host)
		regval = PHY_IDPAD_C_OW;
	else
		regval = PHY_IDPAD_C_OW | PHY_IDPAD_C_SW;

	spin_lock_irqsave(&phy->lock, flags);
	ret = regmap_update_bits(phy->syscon, CV1800_USB_PHY_CTRL_REG,
				 PHY_IDPAD_C_OW | PHY_IDPAD_C_SW,
				 regval);
	spin_unlock_irqrestore(&phy->lock, flags);

	return ret;
}

static void cv1800_usb_phy_set_gpio(struct cv1800_usb_phy *phy,
				    bool is_host)
{
	unsigned int i, ndescs;
	struct gpio_desc **gpios;

	if (!phy->switch_gpios)
		return;

	ndescs = phy->switch_gpios->ndescs;
	gpios = phy->switch_gpios->desc;

	if (is_host) {
		for (i = 0; i < ndescs; i++)
			gpiod_set_value_cansleep(gpios[i], 0);
	} else {
		for (i = 0; i < ndescs; i++)
			gpiod_set_value_cansleep(gpios[ndescs - 1 - i], 1);
	}
}

static int cv1800_usb_phy_set_mode_internal(struct cv1800_usb_phy *phy,
					    bool is_host)
{
	int ret = cv1800_usb_phy_set_idpad(phy, is_host);

	if (ret < 0)
		return ret;

	cv1800_usb_phy_set_gpio(phy, is_host);

	return 0;
}

static ssize_t dr_mode_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct cv1800_usb_phy *phy = dev_get_drvdata(dev);
	bool is_host;

	if (sysfs_streq(buf, "host")) {
		phy->enable_otg = false;
		is_host = true;
	} else if (sysfs_streq(buf, "peripheral")) {
		phy->enable_otg = false;
		is_host = false;
	} else if (sysfs_streq(buf, "otg") && phy->vbus_det_irq > 0) {
		phy->enable_otg = true;
	} else {
		return -EINVAL;
	}

	if (phy->enable_otg)
		queue_delayed_work(system_wq, &phy->vbus_work,
				   PHY_VBUS_DET_DEBOUNCE_TIME);
	else
		cv1800_usb_phy_set_mode_internal(phy, is_host);

	return len;
}

static ssize_t dr_mode_show(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	struct cv1800_usb_phy *phy = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int regval;
	bool is_host = true;
	int ret;

	spin_lock_irqsave(&phy->lock, flags);
	ret = regmap_read(phy->syscon, CV1800_USB_PHY_CTRL_REG, &regval);
	spin_unlock_irqrestore(&phy->lock, flags);

	if (ret)
		return ret;

	if (regval & PHY_IDPAD_C_SW)
		is_host = false;

	return sprintf(buf, "%s%s\n",
		       phy->enable_otg ? "otg: " : "",
		       is_host ? "host" : "peripheral");
}

static DEVICE_ATTR_RW(dr_mode);

static struct attribute *cv1800_usb_phy_attrs[] = {
	&dev_attr_dr_mode.attr,
	NULL
};

static const struct attribute_group cv1800_usb_phy_group = {
	.attrs = cv1800_usb_phy_attrs,
};

static int cv1800_usb_phy_set_clock(struct cv1800_usb_phy *phy)
{
	int ret;

	ret = clk_set_rate(phy->usb_phy_clk, PHY_BASE_CLK_RATE);
	if (ret)
		return ret;

	ret = clk_set_rate(phy->usb_app_clk, PHY_APP_CLK_RATE);
	if (ret)
		return ret;

	ret = clk_set_rate(phy->usb_lpm_clk, PHY_LPM_CLK_RATE);
	if (ret)
		return ret;

	ret = clk_set_rate(phy->usb_stb_clk, PHY_STB_CLK_RATE);
	if (ret)
		return ret;

	return 0;
}

static int cv1800_usb_phy_set_mode(struct phy *_phy,
				   enum phy_mode mode, int submode)
{
	struct cv1800_usb_phy *phy = phy_get_drvdata(_phy);
	bool is_host;

	switch (mode) {
	case PHY_MODE_USB_DEVICE:
		is_host = false;
		phy->enable_otg = false;
		break;
	case PHY_MODE_USB_HOST:
		is_host = true;
		phy->enable_otg = false;
		break;
	case PHY_MODE_USB_OTG:
		/* phy only supports soft OTG when VBUS_DET pin is connected. */
		if (phy->vbus_det_irq > 0) {
			phy->enable_otg = true;
			queue_delayed_work(system_wq, &phy->vbus_work,
					   PHY_VBUS_DET_DEBOUNCE_TIME);
		}
		return 0;
	default:
		return -EINVAL;
	}

	return cv1800_usb_phy_set_mode_internal(phy, is_host);
}

static const struct phy_ops cv1800_usb_phy_ops = {
	.set_mode	= cv1800_usb_phy_set_mode,
	.owner		= THIS_MODULE,
};

static void cv1800_usb_phy_vbus_switch(struct work_struct *work)
{
	struct cv1800_usb_phy *phy =
		container_of(work, struct cv1800_usb_phy, vbus_work.work);
	int state = gpiod_get_value_cansleep(phy->vbus_det_gpio);

	cv1800_usb_phy_set_mode_internal(phy, state == 0);
}

static irqreturn_t cv1800_usb_phy_vbus_det_irq(int irq, void *dev_id)
{
	struct cv1800_usb_phy *phy = dev_id;

	if (phy->enable_otg)
		queue_delayed_work(system_wq, &phy->vbus_work,
				   PHY_VBUS_DET_DEBOUNCE_TIME);
	return IRQ_HANDLED;
}

static void cv1800_usb_phy_init_mode(struct device *dev,
				     struct cv1800_usb_phy *phy)
{
	phy->enable_otg = false;

	if (phy->vbus_det_irq > 0)
		phy->enable_otg = true;

	if (phy->enable_otg)
		queue_delayed_work(system_wq, &phy->vbus_work, 0);
	else
		cv1800_usb_phy_set_mode_internal(phy, true);
}

static int cv1800_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *parent = dev->parent;
	struct cv1800_usb_phy *phy;
	struct phy_provider *phy_provider;
	int ret;

	if (!parent)
		return -ENODEV;

	phy = devm_kmalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->syscon = syscon_node_to_regmap(parent->of_node);
	if (IS_ERR_OR_NULL(phy->syscon))
		return -ENODEV;

	spin_lock_init(&phy->lock);

	phy->usb_phy_clk = devm_clk_get_enabled(dev, "phy");
	if (IS_ERR(phy->usb_phy_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_phy_clk),
			"Failed to get phy clock\n");

	phy->usb_app_clk = devm_clk_get_enabled(dev, "app");
	if (IS_ERR(phy->usb_app_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_app_clk),
			"Failed to get app clock\n");

	phy->usb_lpm_clk = devm_clk_get_enabled(dev, "lpm");
	if (IS_ERR(phy->usb_lpm_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_lpm_clk),
			"Failed to get lpm clock\n");

	phy->usb_stb_clk = devm_clk_get_enabled(dev, "stb");
	if (IS_ERR(phy->usb_stb_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_stb_clk),
			"Failed to get stb clock\n");

	phy->phy = devm_phy_create(dev, NULL, &cv1800_usb_phy_ops);
	if (IS_ERR(phy->phy))
		return dev_err_probe(dev, PTR_ERR(phy->phy),
			"Failed to create phy\n");

	ret = cv1800_usb_phy_set_clock(phy);
	if (ret)
		return ret;

	phy->switch_gpios = devm_gpiod_get_array_optional(dev, "sophgo,switch",
							  GPIOD_OUT_LOW);
	if (IS_ERR(phy->switch_gpios))
		return dev_err_probe(dev, PTR_ERR(phy->switch_gpios),
			"Failed to get switch pin\n");

	phy->vbus_det_gpio = devm_gpiod_get_optional(dev, "vbus_det", GPIOD_IN);
	if (IS_ERR(phy->vbus_det_gpio))
		return dev_err_probe(dev, PTR_ERR(phy->vbus_det_gpio),
			"Failed to process vbus pin\n");

	phy->vbus_det_irq = gpiod_to_irq(phy->vbus_det_gpio);
	if (phy->vbus_det_irq > 0) {
		ret = devm_request_irq(dev, phy->vbus_det_irq,
				       cv1800_usb_phy_vbus_det_irq,
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				       "usb-vbus-det", phy);
		if (ret)
			return dev_err_probe(dev, ret,
				"Failed to request vbus irq\n");
	}

	INIT_DELAYED_WORK(&phy->vbus_work, cv1800_usb_phy_vbus_switch);

	ret = sysfs_create_group(&dev->kobj, &cv1800_usb_phy_group);
	if (ret)
		dev_warn(dev, "failed to create sysfs attributes\n");

	phy_set_drvdata(phy->phy, phy);
	platform_set_drvdata(pdev, phy);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	/*
	 * phy needs to change mode twice after initialization,
	 * otherwise the controller can not found devices attached
	 * to the phy.
	 */
	cv1800_usb_phy_set_idpad(phy, false);
	cv1800_usb_phy_set_idpad(phy, true);
	cv1800_usb_phy_init_mode(dev, phy);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static void cv1800_usb_phy_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cv1800_usb_phy *phy = platform_get_drvdata(pdev);

	if (phy->vbus_det_irq > 0)
		devm_free_irq(dev, phy->vbus_det_irq, phy);

	cancel_delayed_work_sync(&phy->vbus_work);
}

static const struct of_device_id cv1800_usb_phy_ids[] = {
	{ .compatible = "sophgo,cv1800-usb-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, cv1800_usb_phy_ids);

static struct platform_driver cv1800_usb_phy_driver = {
	.probe = cv1800_usb_phy_probe,
	.remove_new = cv1800_usb_phy_remove,
	.driver = {
		.name = "cv1800-usb-phy",
		.of_match_table = cv1800_usb_phy_ids,
	 },
};
module_platform_driver(cv1800_usb_phy_driver);
MODULE_DESCRIPTION("CV1800/SG2000 SoC USB 2.0 PHY driver");
MODULE_LICENSE("GPL");
