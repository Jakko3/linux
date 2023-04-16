// SPDX-License-Identifier: GPL-2.0-only
/*
 * Battery charger driver for RT5033
 *
 * Copyright (C) 2014 Samsung Electronics, Co., Ltd.
 * Author: Beomho Seo <beomho.seo@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/mfd/rt5033-private.h>
#include <linux/mfd/rt5033.h>

static int rt5033_get_charger_state(struct rt5033_charger *charger)
{
	struct regmap *regmap = charger->rt5033->regmap;
	unsigned int reg_data;
	int state;

	if (!regmap)
		return state;

	regmap_read(regmap, RT5033_REG_CHG_STAT, &reg_data);

	switch (reg_data & RT5033_CHG_STAT_MASK) {
	case RT5033_CHG_STAT_DISCHARGING:
		state = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case RT5033_CHG_STAT_CHARGING:
		state = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case RT5033_CHG_STAT_FULL:
		state = POWER_SUPPLY_STATUS_FULL;
		break;
	case RT5033_CHG_STAT_NOT_CHARGING:
		state = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		state = POWER_SUPPLY_STATUS_UNKNOWN;
	}

	return state;
}

static int rt5033_get_charger_type(struct rt5033_charger *charger)
{
	struct regmap *regmap = charger->rt5033->regmap;
	unsigned int reg_data;
	int state;

	regmap_read(regmap, RT5033_REG_CHG_STAT, &reg_data);

	switch (reg_data & RT5033_CHG_STAT_TYPE_MASK) {
	case RT5033_CHG_STAT_TYPE_FAST:
		state = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case RT5033_CHG_STAT_TYPE_PRE:
		state = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	default:
		state = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	return state;
}

static int rt5033_get_charger_current_limit(struct rt5033_charger *charger)
{
	struct regmap *regmap = charger->rt5033->regmap;
	unsigned int state, reg_data, data;

	regmap_read(regmap, RT5033_REG_CHG_CTRL5, &reg_data);

	state = (reg_data & RT5033_CHGCTRL5_ICHG_MASK)
		 >> RT5033_CHGCTRL5_ICHG_SHIFT;

	data = RT5033_CHARGER_FAST_CURRENT_MIN +
		RT5033_CHARGER_FAST_CURRENT_STEP_NUM * state;

	return data;
}

static int rt5033_get_charger_const_voltage(struct rt5033_charger *charger)
{
	struct regmap *regmap = charger->rt5033->regmap;
	unsigned int state, reg_data, data;

	regmap_read(regmap, RT5033_REG_CHG_CTRL2, &reg_data);

	state = (reg_data & RT5033_CHGCTRL2_CV_MASK)
		 >> RT5033_CHGCTRL2_CV_SHIFT;

	data = RT5033_CHARGER_CONST_VOLTAGE_LIMIT_MIN +
		RT5033_CHARGER_CONST_VOLTAGE_STEP_NUM * state;

	return data;
}

static inline int rt5033_init_const_charge(struct rt5033_charger *charger)
{
	struct rt5033_charger_data *chg = charger->chg;
	int ret;
	unsigned int val;
	u8 reg_data;

	/* Set constant voltage mode */
	if (chg->const_uvolt < RT5033_CHARGER_CONST_VOLTAGE_LIMIT_MIN ||
		chg->const_uvolt > RT5033_CHARGER_CONST_VOLTAGE_LIMIT_MAX)
		return -EINVAL;

	if (chg->const_uvolt == RT5033_CHARGER_CONST_VOLTAGE_LIMIT_MIN)
		reg_data = 0x00;
	else if (chg->const_uvolt == RT5033_CHARGER_CONST_VOLTAGE_LIMIT_MAX)
		reg_data = RT5033_CV_MAX_VOLTAGE;
	else {
		val = chg->const_uvolt;
		val -= RT5033_CHARGER_CONST_VOLTAGE_LIMIT_MIN;
		val /= RT5033_CHARGER_CONST_VOLTAGE_STEP_NUM;
		reg_data = val;
	}

	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL2,
			RT5033_CHGCTRL2_CV_MASK,
			reg_data << RT5033_CHGCTRL2_CV_SHIFT);
	if (ret) {
		dev_err(charger->dev, "Failed regmap update\n");
		return -EINVAL;
	}

	/* Set end of charge current */
	if (chg->eoc_uamp < RT5033_CHARGER_EOC_MIN ||
		chg->eoc_uamp > RT5033_CHARGER_EOC_MAX)
		return -EINVAL;

	if (chg->eoc_uamp == RT5033_CHARGER_EOC_MIN)
		reg_data = 0x01;
	else if (chg->eoc_uamp == RT5033_CHARGER_EOC_MAX)
		reg_data = 0x07;
	else {
		val = chg->eoc_uamp;
		if (val < RT5033_CHARGER_EOC_REF) {
			val -= RT5033_CHARGER_EOC_MIN;
			val /= RT5033_CHARGER_EOC_STEP_NUM1;
			reg_data = 0x01 + val;
		} else if (val > RT5033_CHARGER_EOC_REF) {
			val -= RT5033_CHARGER_EOC_REF;
			val /= RT5033_CHARGER_EOC_STEP_NUM2;
			reg_data = 0x04 + val;
		} else {
			reg_data = 0x04;
		}
	}

	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL4,
			RT5033_CHGCTRL4_EOC_MASK, reg_data);
	if (ret) {
		dev_err(charger->dev, "Failed regmap update\n");
		return -EINVAL;
	}

	return 0;
}

static inline int rt5033_init_fast_charge(struct rt5033_charger *charger)
{
	struct rt5033_charger_data *chg = charger->chg;
	int ret;
	unsigned int val;
	u8 reg_data;

	/* Set limit input current */
	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL1,
			RT5033_CHGCTRL1_IAICR_MASK, RT5033_AICR_2000_MODE);
	if (ret) {
		dev_err(charger->dev, "Failed regmap update\n");
		return -EINVAL;
	}

	/* Set fast-charge mode charging current */
	if (chg->fast_uamp < RT5033_CHARGER_FAST_CURRENT_MIN ||
			chg->fast_uamp > RT5033_CHARGER_FAST_CURRENT_MAX)
		return -EINVAL;

	if (chg->fast_uamp == RT5033_CHARGER_FAST_CURRENT_MIN)
		reg_data = 0x00;
	else if (chg->fast_uamp == RT5033_CHARGER_FAST_CURRENT_MAX)
		reg_data = RT5033_CHG_MAX_CURRENT;
	else {
		val = chg->fast_uamp;
		val -= RT5033_CHARGER_FAST_CURRENT_MIN;
		val /= RT5033_CHARGER_FAST_CURRENT_STEP_NUM;
		reg_data = val;
	}

	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL5,
			RT5033_CHGCTRL5_ICHG_MASK,
			reg_data << RT5033_CHGCTRL5_ICHG_SHIFT);
	if (ret) {
		dev_err(charger->dev, "Failed regmap update\n");
		return -EINVAL;
	}

	return 0;
}

static inline int rt5033_init_pre_charge(struct rt5033_charger *charger)
{
	struct rt5033_charger_data *chg = charger->chg;
	int ret;
	unsigned int val;
	u8 reg_data;

	/* Set pre-charge threshold voltage */
	if (chg->pre_uvolt < RT5033_CHARGER_PRE_THRESHOLD_LIMIT_MIN ||
		chg->pre_uvolt > RT5033_CHARGER_PRE_THRESHOLD_LIMIT_MAX)
		return -EINVAL;

	if (chg->pre_uvolt == RT5033_CHARGER_PRE_THRESHOLD_LIMIT_MIN)
		reg_data = 0x00;
	else if (chg->pre_uvolt == RT5033_CHARGER_PRE_THRESHOLD_LIMIT_MAX)
		reg_data = 0x0f;
	else {
		val = chg->pre_uvolt;
		val -= RT5033_CHARGER_PRE_THRESHOLD_LIMIT_MIN;
		val /= RT5033_CHARGER_PRE_THRESHOLD_STEP_NUM;
		reg_data = val;
	}

	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL5,
			RT5033_CHGCTRL5_VPREC_MASK, reg_data);
	if (ret) {
		dev_err(charger->dev, "Failed regmap update\n");
		return -EINVAL;
	}

	/* Set pre-charge mode charging current */
	if (chg->pre_uamp < RT5033_CHARGER_PRE_CURRENT_LIMIT_MIN ||
		chg->pre_uamp > RT5033_CHARGER_PRE_CURRENT_LIMIT_MAX)
		return -EINVAL;

	if (chg->pre_uamp == RT5033_CHARGER_PRE_CURRENT_LIMIT_MIN)
		reg_data = 0x00;
	else if (chg->pre_uamp == RT5033_CHARGER_PRE_CURRENT_LIMIT_MAX)
		reg_data = RT5033_CHG_MAX_PRE_CURRENT;
	else {
		val = chg->pre_uamp;
		val -= RT5033_CHARGER_PRE_CURRENT_LIMIT_MIN;
		val /= RT5033_CHARGER_PRE_CURRENT_STEP_NUM;
		reg_data = val;
	}

	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL4,
			RT5033_CHGCTRL4_IPREC_MASK,
			reg_data << RT5033_CHGCTRL4_IPREC_SHIFT);
	if (ret) {
		dev_err(charger->dev, "Failed regmap update\n");
		return -EINVAL;
	}

	return 0;
}

static int rt5033_charger_reg_init(struct rt5033_charger *charger)
{
	int ret = 0;

	/* Enable charging termination */
	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL1,
			RT5033_CHGCTRL1_TE_EN_MASK, RT5033_TE_ENABLE);
	if (ret) {
		dev_err(charger->dev, "Failed to enable charging termination.\n");
		return -EINVAL;
	}

	/*
	 * Disable minimum input voltage regulation (MIVR), this improves
	 * the charging performance.
	 */
	ret = regmap_update_bits(charger->rt5033->regmap, RT5033_REG_CHG_CTRL4,
			RT5033_CHGCTRL4_MIVR_MASK, RT5033_CHARGER_MIVR_DISABLE);
	if (ret) {
		dev_err(charger->dev, "Failed to disable MIVR.\n");
		return -EINVAL;
	}

	ret = rt5033_init_pre_charge(charger);
	if (ret)
		return ret;

	ret = rt5033_init_fast_charge(charger);
	if (ret)
		return ret;

	ret = rt5033_init_const_charge(charger);
	if (ret)
		return ret;

	return 0;
}

static enum power_supply_property rt5033_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_ONLINE,
};

static int rt5033_charger_get_property(struct power_supply *psy,
			enum power_supply_property psp,
			union power_supply_propval *val)
{
	struct rt5033_charger *charger = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = rt5033_get_charger_state(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = rt5033_get_charger_type(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = rt5033_get_charger_current_limit(charger);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = rt5033_get_charger_const_voltage(charger);
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = RT5033_CHARGER_MODEL;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = RT5033_MANUFACTURER;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = (rt5033_get_charger_state(charger) ==
				POWER_SUPPLY_STATUS_CHARGING);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static struct rt5033_charger_data *rt5033_charger_dt_init(
						struct platform_device *pdev)
{
	struct rt5033_charger_data *chg;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	if (!np) {
		dev_err(&pdev->dev, "No charger of_node\n");
		return ERR_PTR(-EINVAL);
	}

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32(np, "richtek,pre-microamp", &chg->pre_uamp);
	if (ret)
		return ERR_PTR(ret);

	ret = of_property_read_u32(np, "richtek,pre-threshold-microvolt",
			&chg->pre_uvolt);
	if (ret)
		return ERR_PTR(ret);

	ret = of_property_read_u32(np, "richtek,fast-microamp", &chg->fast_uamp);
	if (ret)
		return ERR_PTR(ret);

	ret = of_property_read_u32(np, "richtek,const-microvolt",
			&chg->const_uvolt);
	if (ret)
		return ERR_PTR(ret);

	ret = of_property_read_u32(np, "richtek,eoc-microamp", &chg->eoc_uamp);
	if (ret)
		return ERR_PTR(ret);

	return chg;
}

static const struct power_supply_desc rt5033_charger_desc = {
	.name = "rt5033-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = rt5033_charger_props,
	.num_properties = ARRAY_SIZE(rt5033_charger_props),
	.get_property = rt5033_charger_get_property,
};

static int rt5033_charger_probe(struct platform_device *pdev)
{
	struct rt5033_charger *charger;
	struct rt5033_dev *rt5033 = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	int ret;

	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	platform_set_drvdata(pdev, charger);
	charger->dev = &pdev->dev;
	charger->rt5033 = rt5033;

	charger->chg = rt5033_charger_dt_init(pdev);
	if (IS_ERR_OR_NULL(charger->chg))
		return -ENODEV;

	ret = rt5033_charger_reg_init(charger);
	if (ret)
		return ret;

	psy_cfg.of_node = pdev->dev.of_node;
	psy_cfg.drv_data = charger;

	charger->psy = devm_power_supply_register(&pdev->dev,
						  &rt5033_charger_desc,
						  &psy_cfg);
	if (IS_ERR(charger->psy)) {
		dev_err(&pdev->dev, "failed: power supply register\n");
		return PTR_ERR(charger->psy);
	}

	return 0;
}

static const struct platform_device_id rt5033_charger_id[] = {
	{ "rt5033-charger", },
	{ }
};
MODULE_DEVICE_TABLE(platform, rt5033_charger_id);

static const struct of_device_id rt5033_charger_of_match[] = {
	{ .compatible = "richtek,rt5033-charger", },
	{ }
};
MODULE_DEVICE_TABLE(of, rt5033_charger_of_match);

static struct platform_driver rt5033_charger_driver = {
	.driver = {
		.name = "rt5033-charger",
		.of_match_table = rt5033_charger_of_match,
	},
	.probe = rt5033_charger_probe,
	.id_table = rt5033_charger_id,
};
module_platform_driver(rt5033_charger_driver);

MODULE_DESCRIPTION("Richtek RT5033 charger driver");
MODULE_AUTHOR("Beomho Seo <beomho.seo@samsung.com>");
MODULE_LICENSE("GPL v2");
