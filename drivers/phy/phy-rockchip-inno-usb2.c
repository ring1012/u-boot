/*
 * Copyright 2017 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:    GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <dm/lists.h>
#include <generic-phy.h>
#include <linux/ioport.h>
#include <power/regulator.h>
#include <regmap.h>
#include <syscon.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/cpu.h>
#include <asm/gpio.h>
#include <reset-uclass.h>

#include "../usb/gadget/dwc2_udc_otg_priv.h"

#define U2PHY_BIT_WRITEABLE_SHIFT	16
#define CHG_DCD_MAX_RETRIES		6
#define CHG_PRI_MAX_RETRIES		2
#define CHG_DCD_POLL_TIME		100	/* millisecond */
#define CHG_PRIMARY_DET_TIME		40	/* millisecond */
#define CHG_SECONDARY_DET_TIME		40	/* millisecond */

struct rockchip_usb2phy;

enum power_supply_type {
	POWER_SUPPLY_TYPE_UNKNOWN = 0,
	POWER_SUPPLY_TYPE_USB,		/* Standard Downstream Port */
	POWER_SUPPLY_TYPE_USB_DCP,	/* Dedicated Charging Port */
	POWER_SUPPLY_TYPE_USB_CDP,	/* Charging Downstream Port */
	POWER_SUPPLY_TYPE_USB_FLOATING,	/* DCP without shorting D+/D- */
};

enum rockchip_usb2phy_port_id {
	USB2PHY_PORT_OTG,
	USB2PHY_PORT_HOST,
	USB2PHY_NUM_PORTS,
};

struct usb2phy_reg {
	u32	offset;
	u32	bitend;
	u32	bitstart;
	u32	disable;
	u32	enable;
};

/**
 * struct rockchip_chg_det_reg: usb charger detect registers
 * @cp_det: charging port detected successfully.
 * @dcp_det: dedicated charging port detected successfully.
 * @dp_det: assert data pin connect successfully.
 * @idm_sink_en: open dm sink curren.
 * @idp_sink_en: open dp sink current.
 * @idp_src_en: open dm source current.
 * @rdm_pdwn_en: open dm pull down resistor.
 * @vdm_src_en: open dm voltage source.
 * @vdp_src_en: open dp voltage source.
 * @opmode: utmi operational mode.
 */
struct rockchip_chg_det_reg {
	struct usb2phy_reg	cp_det;
	struct usb2phy_reg	dcp_det;
	struct usb2phy_reg	dp_det;
	struct usb2phy_reg	idm_sink_en;
	struct usb2phy_reg	idp_sink_en;
	struct usb2phy_reg	idp_src_en;
	struct usb2phy_reg	rdm_pdwn_en;
	struct usb2phy_reg	vdm_src_en;
	struct usb2phy_reg	vdp_src_en;
	struct usb2phy_reg	opmode;
};

/**
 * struct rockchip_usb2phy_port_cfg: usb-phy port configuration.
 * @phy_sus: phy suspend register.
 * @bvalid_det_en: vbus valid rise detection enable register.
 * @bvalid_det_st: vbus valid rise detection status register.
 * @bvalid_det_clr: vbus valid rise detection clear register.
 * @ls_det_en: linestate detection enable register.
 * @ls_det_st: linestate detection state register.
 * @ls_det_clr: linestate detection clear register.
 * @iddig_output: iddig output from grf.
 * @iddig_en: utmi iddig select between grf and phy,
 *	      0: from phy; 1: from grf
 * @idfall_det_en: id fall detection enable register.
 * @idfall_det_st: id fall detection state register.
 * @idfall_det_clr: id fall detection clear register.
 * @idrise_det_en: id rise detection enable register.
 * @idrise_det_st: id rise detection state register.
 * @idrise_det_clr: id rise detection clear register.
 * @utmi_avalid: utmi vbus avalid status register.
 * @utmi_bvalid: utmi vbus bvalid status register.
 * @utmi_iddig: otg port id pin status register.
 * @utmi_ls: utmi linestate state register.
 * @utmi_hstdet: utmi host disconnect register.
 * @vbus_det_en: vbus detect function power down register.
 */
struct rockchip_usb2phy_port_cfg {
	struct usb2phy_reg	phy_sus;
	struct usb2phy_reg	bvalid_det_en;
	struct usb2phy_reg	bvalid_det_st;
	struct usb2phy_reg	bvalid_det_clr;
	struct usb2phy_reg	ls_det_en;
	struct usb2phy_reg	ls_det_st;
	struct usb2phy_reg	ls_det_clr;
	struct usb2phy_reg	iddig_output;
	struct usb2phy_reg	iddig_en;
	struct usb2phy_reg	idfall_det_en;
	struct usb2phy_reg	idfall_det_st;
	struct usb2phy_reg	idfall_det_clr;
	struct usb2phy_reg	idrise_det_en;
	struct usb2phy_reg	idrise_det_st;
	struct usb2phy_reg	idrise_det_clr;
	struct usb2phy_reg	utmi_avalid;
	struct usb2phy_reg	utmi_bvalid;
	struct usb2phy_reg	utmi_iddig;
	struct usb2phy_reg	utmi_ls;
	struct usb2phy_reg	utmi_hstdet;
	struct usb2phy_reg	vbus_det_en;
};

/**
 * struct rockchip_usb2phy_cfg: usb-phy configuration.
 * @reg: the address offset of grf for usb-phy config.
 * @num_ports: specify how many ports that the phy has.
 * @phy_tuning: phy default parameters tunning.
 * @clkout_ctl: keep on/turn off output clk of phy.
 * @chg_det: charger detection registers.
 */
struct rockchip_usb2phy_cfg {
	u32	reg;
	u32	num_ports;
	int (*phy_tuning)(struct rockchip_usb2phy *);
	struct usb2phy_reg	clkout_ctl;
	const struct rockchip_usb2phy_port_cfg	port_cfgs[USB2PHY_NUM_PORTS];
	const struct rockchip_chg_det_reg	chg_det;
};

/**
 * @dcd_retries: The retry count used to track Data contact
 *		 detection process.
 * @primary_retries: The retry count used to do usb bc detection
 *		     primary stage.
 * @grf: General Register Files register base.
 * @usbgrf_base : USB General Register Files register base.
 * @phy_base: the base address of USB PHY.
 * @phy_rst: phy reset control.
 * @vbus_det_gpio: VBUS detection via GPIO.
 * @phy_cfg: phy register configuration, assigned by driver data.
 */
struct rockchip_usb2phy {
	u8		dcd_retries;
	u8		primary_retries;
	struct regmap	*grf_base;
	struct regmap	*usbgrf_base;
	void __iomem	*phy_base;
	struct udevice	*vbus_supply[USB2PHY_NUM_PORTS];
	struct reset_ctl phy_rst;
	struct gpio_desc vbus_det_gpio;
	const struct rockchip_usb2phy_cfg	*phy_cfg;
};

static inline struct regmap *get_reg_base(struct rockchip_usb2phy *rphy)
{
	return !rphy->usbgrf_base ? rphy->grf_base : rphy->usbgrf_base;
}

static inline int property_enable(struct regmap *base,
				  const struct usb2phy_reg *reg, bool en)
{
	u32 val, mask, tmp;

	tmp = en ? reg->enable : reg->disable;
	mask = GENMASK(reg->bitend, reg->bitstart);
	val = (tmp << reg->bitstart) | (mask << U2PHY_BIT_WRITEABLE_SHIFT);

	return regmap_write(base, reg->offset, val);
}

static inline bool property_enabled(struct regmap *base,
				    const struct usb2phy_reg *reg)
{
	u32 tmp, orig;
	u32 mask = GENMASK(reg->bitend, reg->bitstart);

	regmap_read(base, reg->offset, &orig);

	tmp = (orig & mask) >> reg->bitstart;

	return tmp == reg->enable;
}

static inline void phy_clear_bits(void __iomem *reg, u32 bits)
{
	u32 tmp = readl(reg);

	tmp &= ~bits;
	writel(tmp, reg);
}

static inline void phy_set_bits(void __iomem *reg, u32 bits)
{
	u32 tmp = readl(reg);

	tmp |= bits;
	writel(tmp, reg);
}

static inline void phy_update_bits(void __iomem *reg, u32 mask, u32 val)
{
	u32 tmp = readl(reg);

	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, reg);
}

static const char *chg_to_string(enum power_supply_type chg_type)
{
	switch (chg_type) {
	case POWER_SUPPLY_TYPE_USB:
		return "USB_SDP_CHARGER";
	case POWER_SUPPLY_TYPE_USB_DCP:
		return "USB_DCP_CHARGER";
	case POWER_SUPPLY_TYPE_USB_CDP:
		return "USB_CDP_CHARGER";
	case POWER_SUPPLY_TYPE_USB_FLOATING:
		return "USB_FLOATING_CHARGER";
	default:
		return "INVALID_CHARGER";
	}
}

static void rockchip_chg_enable_dcd(struct rockchip_usb2phy *rphy,
				    bool en)
{
	struct regmap *base = get_reg_base(rphy);

	property_enable(base, &rphy->phy_cfg->chg_det.rdm_pdwn_en, en);
	property_enable(base, &rphy->phy_cfg->chg_det.idp_src_en, en);
}

static void rockchip_chg_enable_primary_det(struct rockchip_usb2phy *rphy,
					    bool en)
{
	struct regmap *base = get_reg_base(rphy);

	property_enable(base, &rphy->phy_cfg->chg_det.vdp_src_en, en);
	property_enable(base, &rphy->phy_cfg->chg_det.idm_sink_en, en);
}

static void rockchip_chg_enable_secondary_det(struct rockchip_usb2phy *rphy,
					      bool en)
{
	struct regmap *base = get_reg_base(rphy);

	property_enable(base, &rphy->phy_cfg->chg_det.vdm_src_en, en);
	property_enable(base, &rphy->phy_cfg->chg_det.idp_sink_en, en);
}

static bool rockchip_chg_primary_det_retry(struct rockchip_usb2phy *rphy)
{
	bool vout = false;
	struct regmap *base = get_reg_base(rphy);

	while (rphy->primary_retries--) {
		/* voltage source on DP, probe on DM */
		rockchip_chg_enable_primary_det(rphy, true);
		mdelay(CHG_PRIMARY_DET_TIME);
		vout = property_enabled(base, &rphy->phy_cfg->chg_det.cp_det);
		if (vout)
			break;
	}

	rockchip_chg_enable_primary_det(rphy, false);
	return vout;
}

#ifdef CONFIG_ROCKCHIP_RK3506
static void rockchip_u2phy_get_vbus_gpio(struct udevice *dev)
{
	ofnode otg_node, extcon_usb_node;
	struct rockchip_usb2phy *rphy = dev_get_priv(dev);

	rphy->vbus_det_gpio.dev = NULL;
	otg_node = dev_read_subnode(dev, "otg-port");
	if (!ofnode_valid(otg_node)) {
		debug("%s: %s otg subnode not found!\n", __func__, dev->name);
		return;
	}

	if (ofnode_read_bool(otg_node, "rockchip,gpio-vbus-det")) {
		extcon_usb_node = ofnode_path("/extcon-usb");
		if (!ofnode_valid(extcon_usb_node)) {
			debug("%s: extcon-usb node not found\n", __func__);
			return;
		}

		gpio_request_by_name_nodev(extcon_usb_node, "vbus-gpio", 0,
					   &rphy->vbus_det_gpio, GPIOD_IS_IN);
	}
}
#endif

int rockchip_chg_get_type(void)
{
	const struct rockchip_usb2phy_port_cfg *port_cfg;
	enum power_supply_type chg_type;
	struct rockchip_usb2phy *rphy;
	struct udevice *udev;
	struct regmap *base;
	bool is_dcd, vout;
	int ret;

	ret = uclass_get_device_by_name(UCLASS_PHY, "usb2-phy", &udev);
	if (ret == -ENODEV) {
		ret = uclass_get_device_by_name(UCLASS_PHY, "usb2phy", &udev);
		if (ret) {
			pr_err("%s: get usb2 phy node failed: %d\n", __func__, ret);
			return ret;
		}
	}

	rphy = dev_get_priv(udev);
	base = get_reg_base(rphy);
	port_cfg = &rphy->phy_cfg->port_cfgs[USB2PHY_PORT_OTG];

#ifdef CONFIG_ROCKCHIP_RK3506
	rockchip_u2phy_get_vbus_gpio(udev);
#else
	rphy->vbus_det_gpio.dev = NULL;
#endif

	/* Check USB-Vbus status first */
	if (dm_gpio_is_valid(&rphy->vbus_det_gpio)) {
		if (dm_gpio_get_value(&rphy->vbus_det_gpio)) {
			pr_info("%s: vbus gpio voltage valid\n", __func__);
		} else {
			pr_info("%s: vbus gpio voltage invalid\n", __func__);
			return POWER_SUPPLY_TYPE_UNKNOWN;
		}
	} else if (!property_enabled(base, &port_cfg->utmi_bvalid)) {
		pr_info("%s: no charger found\n", __func__);
		return POWER_SUPPLY_TYPE_UNKNOWN;
	}

#ifdef CONFIG_ROCKCHIP_RK3036
	chg_type = POWER_SUPPLY_TYPE_USB;
	goto out;
#endif

	/* Suspend USB-PHY and put the controller in non-driving mode */
	property_enable(base, &port_cfg->phy_sus, true);
	property_enable(base, &rphy->phy_cfg->chg_det.opmode, false);

	rphy->dcd_retries = CHG_DCD_MAX_RETRIES;
	rphy->primary_retries = CHG_PRI_MAX_RETRIES;

	/* stage 1, start DCD processing stage */
	rockchip_chg_enable_dcd(rphy, true);

	while (rphy->dcd_retries--) {
		mdelay(CHG_DCD_POLL_TIME);

		/* get data contact detection status */
		is_dcd = property_enabled(base, &rphy->phy_cfg->chg_det.dp_det);

		if (is_dcd || !rphy->dcd_retries) {
			/*
			 * stage 2, turn off DCD circuitry, then
			 * voltage source on DP, probe on DM.
			 */
			rockchip_chg_enable_dcd(rphy, false);
			rockchip_chg_enable_primary_det(rphy, true);
			break;
		}
	}

	mdelay(CHG_PRIMARY_DET_TIME);
	vout = property_enabled(base, &rphy->phy_cfg->chg_det.cp_det);
	rockchip_chg_enable_primary_det(rphy, false);
	if (vout) {
		/* stage 3, voltage source on DM, probe on DP */
		rockchip_chg_enable_secondary_det(rphy, true);
	} else {
		if (!rphy->dcd_retries) {
			/* floating charger found */
			chg_type = POWER_SUPPLY_TYPE_USB_FLOATING;
			goto out;
		} else {
			/*
			 * Retry some times to make sure that it's
			 * really a USB SDP charger.
			 */
			vout = rockchip_chg_primary_det_retry(rphy);
			if (vout) {
				/* stage 3, voltage source on DM, probe on DP */
				rockchip_chg_enable_secondary_det(rphy, true);
			} else {
				/* USB SDP charger found */
				chg_type = POWER_SUPPLY_TYPE_USB;
				goto out;
			}
		}
	}

	mdelay(CHG_SECONDARY_DET_TIME);
	vout = property_enabled(base, &rphy->phy_cfg->chg_det.dcp_det);
	/* stage 4, turn off voltage source */
	rockchip_chg_enable_secondary_det(rphy, false);
	if (vout)
		chg_type = POWER_SUPPLY_TYPE_USB_DCP;
	else
		chg_type = POWER_SUPPLY_TYPE_USB_CDP;

out:
	/* Resume USB-PHY and put the controller in normal mode */
	property_enable(base, &rphy->phy_cfg->chg_det.opmode, true);
	property_enable(base, &port_cfg->phy_sus, false);

	debug("charger is %s\n", chg_to_string(chg_type));

	return chg_type;
}

int rockchip_u2phy_vbus_detect(void)
{
	int chg_type;

	chg_type = rockchip_chg_get_type();

	return (chg_type == POWER_SUPPLY_TYPE_USB ||
		chg_type == POWER_SUPPLY_TYPE_USB_CDP) ? 1 : 0;
}

void otg_phy_init(struct dwc2_udc *dev)
{
	const struct rockchip_usb2phy_port_cfg *port_cfg;
	struct rockchip_usb2phy *rphy;
	struct udevice *udev;
	struct regmap *base;
	int ret;

	ret = uclass_get_device_by_name(UCLASS_PHY, "usb2-phy", &udev);
	if (ret == -ENODEV) {
		ret = uclass_get_device_by_name(UCLASS_PHY, "usb2phy", &udev);
		if (ret) {
			pr_err("%s: get usb2 phy node failed: %d\n", __func__, ret);
			return;
		}
	}

	rphy = dev_get_priv(udev);
	base = get_reg_base(rphy);
	port_cfg = &rphy->phy_cfg->port_cfgs[USB2PHY_PORT_OTG];

	/* Set the USB-PHY COMMONONN to 1'b0 to ensure USB's clocks */
	if(rphy->phy_cfg->clkout_ctl.disable)
		property_enable(base, &rphy->phy_cfg->clkout_ctl, true);

	/* Reset USB-PHY */
	property_enable(base, &port_cfg->phy_sus, true);
	udelay(20);
	property_enable(base, &port_cfg->phy_sus, false);
	mdelay(2);
}

static int rockchip_usb2phy_reset(struct rockchip_usb2phy *rphy)
{
	int ret;

	if (rphy->phy_rst.dev) {
		ret = reset_assert(&rphy->phy_rst);
		if (ret < 0) {
			pr_err("u2phy assert reset failed: %d", ret);
			return ret;
		}

		udelay(20);

		ret = reset_deassert(&rphy->phy_rst);
		if (ret < 0) {
			pr_err("u2phy deassert reset failed: %d", ret);
			return ret;
		}

		udelay(100);
	}

	return 0;
}

static int rockchip_usb2phy_init(struct phy *phy)
{
	struct udevice *parent = phy->dev->parent;
	struct rockchip_usb2phy *rphy = dev_get_priv(parent);
	const struct rockchip_usb2phy_port_cfg *port_cfg;
	struct regmap *base = get_reg_base(rphy);

	if (phy->id == USB2PHY_PORT_OTG) {
		port_cfg = &rphy->phy_cfg->port_cfgs[USB2PHY_PORT_OTG];
	} else if (phy->id == USB2PHY_PORT_HOST) {
		port_cfg = &rphy->phy_cfg->port_cfgs[USB2PHY_PORT_HOST];
	} else {
		dev_err(phy->dev, "phy id %lu not support", phy->id);
		return -EINVAL;
	}

	property_enable(base, &port_cfg->phy_sus, false);

	/* waiting for the utmi_clk to become stable */
	udelay(2000);

	return 0;
}

static int rockchip_usb2phy_exit(struct phy *phy)
{
	struct udevice *parent = phy->dev->parent;
	struct rockchip_usb2phy *rphy = dev_get_priv(parent);
	const struct rockchip_usb2phy_port_cfg *port_cfg;
	struct regmap *base = get_reg_base(rphy);

	if (phy->id == USB2PHY_PORT_OTG) {
		port_cfg = &rphy->phy_cfg->port_cfgs[USB2PHY_PORT_OTG];
	} else if (phy->id == USB2PHY_PORT_HOST) {
		port_cfg = &rphy->phy_cfg->port_cfgs[USB2PHY_PORT_HOST];
	} else {
		dev_err(phy->dev, "phy id %lu not support", phy->id);
		return -EINVAL;
	}

	property_enable(base, &port_cfg->phy_sus, true);

	return 0;
}

static int rockchip_usb2phy_power_on(struct phy *phy)
{
	struct udevice *parent = phy->dev->parent;
	struct rockchip_usb2phy *rphy = dev_get_priv(parent);
	struct udevice *vbus = rphy->vbus_supply[phy->id];
	int ret;

	if (vbus) {
		ret = regulator_set_enable(vbus, true);
		if (ret) {
			pr_err("%s: Failed to set VBus supply\n", __func__);
			return ret;
		}
	}

	return 0;
}

static int rockchip_usb2phy_power_off(struct phy *phy)
{
	struct udevice *parent = phy->dev->parent;
	struct rockchip_usb2phy *rphy = dev_get_priv(parent);
	struct udevice *vbus = rphy->vbus_supply[phy->id];
	int ret;

	if (vbus) {
		ret = regulator_set_enable(vbus, false);
		if (ret) {
			pr_err("%s: Failed to set VBus supply\n", __func__);
			return ret;
		}
	}

	return 0;
}

static int rockchip_usb2phy_of_xlate(struct phy *phy,
				     struct ofnode_phandle_args *args)
{
	const char *dev_name = phy->dev->name;
	struct udevice *parent = phy->dev->parent;
	struct rockchip_usb2phy *rphy = dev_get_priv(parent);

	if (!strcasecmp(dev_name, "host-port")) {
		phy->id = USB2PHY_PORT_HOST;
		device_get_supply_regulator(phy->dev, "phy-supply",
					    &rphy->vbus_supply[USB2PHY_PORT_HOST]);
	} else if (!strcasecmp(dev_name, "otg-port")) {
		phy->id = USB2PHY_PORT_OTG;
		device_get_supply_regulator(phy->dev, "phy-supply",
					    &rphy->vbus_supply[USB2PHY_PORT_OTG]);
		if (!rphy->vbus_supply[USB2PHY_PORT_OTG])
			device_get_supply_regulator(phy->dev, "vbus-supply",
						    &rphy->vbus_supply[USB2PHY_PORT_OTG]);
	} else {
		pr_err("%s: invalid dev name\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int rockchip_usb2phy_bind(struct udevice *dev)
{
	struct udevice *child;
	ofnode subnode;
	const char *node_name;
	int ret;

	dev_for_each_subnode(subnode, dev) {
		if (!ofnode_valid(subnode)) {
			debug("%s: %s subnode not found", __func__, dev->name);
			return -ENXIO;
		}

		node_name = ofnode_get_name(subnode);
		debug("%s: subnode %s\n", __func__, node_name);

		ret = device_bind_driver_to_node(dev, "rockchip_usb2phy_port",
						 node_name, subnode, &child);
		if (ret) {
			pr_err("%s: '%s' cannot bind 'rockchip_usb2phy_port'\n",
			       __func__, node_name);
			return ret;
		}
	}

	return 0;
}

static int rockchip_usb2phy_probe(struct udevice *dev)
{
	const struct rockchip_usb2phy_cfg *phy_cfgs;
	struct rockchip_usb2phy *rphy = dev_get_priv(dev);
	struct udevice *parent = dev->parent;
	struct udevice *syscon;
	struct resource res;
	u32 reg, index;
	int ret;

	rphy->phy_base = (void __iomem *)dev_read_addr(dev);
	if (IS_ERR(rphy->phy_base)) {
		dev_err(dev, "get the base address of usb phy failed\n");
	}

	if (!strncmp(parent->name, "root_driver", 11) &&
	    dev_read_bool(dev, "rockchip,grf")) {
		ret = uclass_get_device_by_phandle(UCLASS_SYSCON, dev,
						   "rockchip,grf", &syscon);
		if (ret) {
			dev_err(dev, "get syscon grf failed\n");
			return ret;
		}

		rphy->grf_base = syscon_get_regmap(syscon);
	} else {
		rphy->grf_base = syscon_get_regmap(parent);
	}

	if (rphy->grf_base <= 0) {
		dev_err(dev, "get syscon grf regmap failed\n");
		return -EINVAL;
	}

	if (dev_read_bool(dev, "rockchip,usbgrf")) {
		ret = uclass_get_device_by_phandle(UCLASS_SYSCON, dev,
						   "rockchip,usbgrf", &syscon);
		if (ret) {
			dev_err(dev, "get syscon usbgrf failed\n");
			return ret;
		}

		rphy->usbgrf_base = syscon_get_regmap(syscon);
		if (rphy->usbgrf_base <= 0) {
			dev_err(dev, "get syscon usbgrf regmap failed\n");
			return -EINVAL;
		}
	} else {
		rphy->usbgrf_base = NULL;
	}

	if (!strncmp(parent->name, "root_driver", 11)) {
		ret = dev_read_resource(dev, 0, &res);
		reg = res.start;
	} else {
		ret = ofnode_read_u32(dev_ofnode(dev), "reg", &reg);
	}

	if (ret) {
		dev_err(dev, "could not read reg\n");
		return -EINVAL;
	}

	ret = reset_get_by_name(dev, "phy", &rphy->phy_rst);
	if (ret)
		dev_dbg(dev, "no u2phy reset control specified\n");

	phy_cfgs =
		(const struct rockchip_usb2phy_cfg *)dev_get_driver_data(dev);
	if (!phy_cfgs) {
		dev_err(dev, "unable to get phy_cfgs\n");
		return -EINVAL;
	}

	/* find out a proper config which can be matched with dt. */
	index = 0;
	do {
		if (phy_cfgs[index].reg == reg) {
			rphy->phy_cfg = &phy_cfgs[index];
			break;
		}
		++index;
	} while (phy_cfgs[index].reg);

	if (!rphy->phy_cfg) {
		dev_err(dev, "no phy-config can be matched\n");
		return -EINVAL;
	}

	if (rphy->phy_cfg->phy_tuning)
		rphy->phy_cfg->phy_tuning(rphy);

	return 0;
}

static int rk322x_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	struct regmap *base = get_reg_base(rphy);
	int ret = 0;

	/* Open pre-emphasize in non-chirp state for PHY0 otg port */
	if (rphy->phy_cfg->reg == 0x760)
		ret = regmap_write(base, 0x76c, 0x00070004);

	return ret;
}

static int rk3308_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	struct regmap *base = get_reg_base(rphy);
	unsigned int tmp, orig;
	int ret;

	if (soc_is_rk3308bs()) {
		/* Enable otg/host port pre-emphasis during non-chirp phase */
		ret = regmap_read(base, 0, &orig);
		if (ret)
			return ret;
		tmp = orig & ~GENMASK(2, 0);
		tmp |= BIT(2) & GENMASK(2, 0);
		ret = regmap_write(base, 0, tmp);
		if (ret)
			return ret;

		/* Set otg port squelch trigger point configure to 100mv */
		ret = regmap_read(base, 0x004, &orig);
		if (ret)
			return ret;
		tmp = orig & ~GENMASK(7, 5);
		tmp |= 0x40 & GENMASK(7, 5);
		ret = regmap_write(base, 0x004, tmp);
		if (ret)
			return ret;

		ret = regmap_read(base, 0x008, &orig);
		if (ret)
			return ret;
		tmp = orig & ~BIT(0);
		tmp |= 0x1 & BIT(0);
		ret = regmap_write(base, 0x008, tmp);
		if (ret)
			return ret;

		/* Enable host port pre-emphasis during non-chirp phase */
		ret = regmap_read(base, 0x400, &orig);
		if (ret)
			return ret;
		tmp = orig & ~GENMASK(2, 0);
		tmp |= BIT(2) & GENMASK(2, 0);
		ret = regmap_write(base, 0x400, tmp);
		if (ret)
			return ret;

		/* Set host port squelch trigger point configure to 100mv */
		ret = regmap_read(base, 0x404, &orig);
		if (ret)
			return ret;
		tmp = orig & ~GENMASK(7, 5);
		tmp |= 0x40 & GENMASK(7, 5);
		ret = regmap_write(base, 0x404, tmp);
		if (ret)
			return ret;

		ret = regmap_read(base, 0x408, &orig);
		if (ret)
			return ret;
		tmp = orig & ~BIT(0);
		tmp |= 0x1 & BIT(0);
		ret = regmap_write(base, 0x408, tmp);
		if (ret)
			return ret;
	}

	return 0;
}

static int rk3328_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	struct regmap *base = get_reg_base(rphy);
	int ret;

	if (soc_is_px30s()) {
		/* Enable otg/host port pre-emphasis during non-chirp phase */
		ret = regmap_update_bits(base, 0x8000, GENMASK(2, 0), BIT(2));
		if (ret)
			return ret;

		/* Set otg port squelch trigger point configure to 100mv */
		ret = regmap_update_bits(base, 0x8004, GENMASK(7, 5), 0x40);
		if (ret)
			return ret;

		ret = regmap_update_bits(base, 0x8008, BIT(0), 0x1);
		if (ret)
			return ret;

		/* Enable host port pre-emphasis during non-chirp phase */
		ret = regmap_update_bits(base, 0x8400, GENMASK(2, 0), BIT(2));
		if (ret)
			return ret;

		/* Set host port squelch trigger point configure to 100mv */
		ret = regmap_update_bits(base, 0x8404, GENMASK(7, 5), 0x40);
		if (ret)
			return ret;

		ret = regmap_update_bits(base, 0x8408, BIT(0), 0x1);
		if (ret)
			return ret;
	} else {
		/* Open debug mode for tuning */
		ret = regmap_write(base, 0x2c, 0xffff0400);
		if (ret)
			return ret;

		/* Open pre-emphasize in non-chirp state for otg port */
		ret = regmap_write(base, 0x0, 0x00070004);
		if (ret)
			return ret;

		/* Open pre-emphasize in non-chirp state for host port */
		ret = regmap_write(base, 0x30, 0x00070004);
		if (ret)
			return ret;
	}

	return 0;
}

static int rv1103b_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	/* Always enable pre-emphasis in SOF & EOP & chirp & non-chirp state */
	phy_update_bits(rphy->phy_base + 0x30, GENMASK(2, 0), 0x07);

	/* Set Tx HS pre_emphasize strength to 3'b001 */
	phy_update_bits(rphy->phy_base + 0x40, GENMASK(5, 3), (0x01 << 3));

	/* Set RX Squelch trigger point configure to 4'b0000(112.5 mV) */
	phy_update_bits(rphy->phy_base + 0x64, GENMASK(6, 3), (0x00 << 3));

	/* Turn off differential receiver by default to save power */
	phy_clear_bits(rphy->phy_base + 0x100, BIT(6));

	/* Set 45ohm HS ODT value to 5'b10111 to increase driver strength */
	phy_update_bits(rphy->phy_base + 0x11c, GENMASK(4, 0), 0x17);

	/* Set Tx HS eye height tuning to 3'b011(462 mV)*/
	phy_update_bits(rphy->phy_base + 0x124, GENMASK(4, 2), (0x03 << 2));

	/* Bypass Squelch detector calibration */
	phy_update_bits(rphy->phy_base + 0x1a4, GENMASK(7, 4), (0x01 << 4));
	phy_update_bits(rphy->phy_base + 0x1b4, GENMASK(7, 4), (0x01 << 4));

	/* Set HS disconnect detect mode to single ended detect mode */
	phy_set_bits(rphy->phy_base + 0x70, BIT(2));

	/* Set Host Disconnect Detection to 675mV */
	phy_update_bits(rphy->phy_base + 0x60, GENMASK(1, 0), 0x0);
	phy_update_bits(rphy->phy_base + 0x64, GENMASK(7, 7), BIT(7));
	phy_update_bits(rphy->phy_base + 0x68, GENMASK(0, 0), 0x0);

	return 0;
}

static int rv1106_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	/* Set HS disconnect detect mode to single ended detect mode */
	phy_set_bits(rphy->phy_base + 0x70, BIT(2));

	return 0;
}

static int rk3506_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	/* Turn off otg0 port differential receiver in suspend mode */
	phy_clear_bits(rphy->phy_base + 0x30, BIT(2));

	/* Turn off otg1 port differential receiver in suspend mode */
	phy_clear_bits(rphy->phy_base + 0x430, BIT(2));

	/* Set otg0 port HS eye height to 425mv(default is 450mv) */
	phy_update_bits(rphy->phy_base + 0x30, GENMASK(6, 4), (0x05 << 4));

	/* Set otg1 port HS eye height to 425mv(default is 450mv) */
	phy_update_bits(rphy->phy_base + 0x430, GENMASK(6, 4), (0x05 << 4));

	/* Choose the Tx fs/ls data as linestate from TX driver for otg0 port */
	phy_update_bits(rphy->phy_base + 0x94, GENMASK(6, 3), (0x03 << 3));

	/* Choose the Tx fs/ls data as linestate from TX driver for otg1 port */
	phy_update_bits(rphy->phy_base + 0x494, GENMASK(6, 3), (0x03 << 3));

	return 0;
}

static int rk3528_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	if (IS_ERR(rphy->phy_base)) {
		return PTR_ERR(rphy->phy_base);
	}

	/* Turn off otg port differential receiver in suspend mode */
	phy_clear_bits(rphy->phy_base + 0x30, BIT(2));

	/* Turn off host port differential receiver in suspend mode */
	phy_clear_bits(rphy->phy_base + 0x430, BIT(2));

	/* Set otg port HS eye height to 400mv(default is 450mv) */
	phy_update_bits(rphy->phy_base + 0x30, GENMASK(6, 4), (0x00 << 4));

	/* Set host port HS eye height to 400mv(default is 450mv) */
	phy_update_bits(rphy->phy_base + 0x430, GENMASK(6, 4), (0x00 << 4));

	/* Choose the Tx fs/ls data as linestate from TX driver for otg port */
	phy_update_bits(rphy->phy_base + 0x94, GENMASK(6, 3), (0x03 << 3));

	/* Turn on output clk of phy*/
	phy_update_bits(rphy->phy_base + 0x41c, GENMASK(7, 2), (0x27 << 2));

	return 0;
}

static int rk3562_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	if (IS_ERR(rphy->phy_base)) {
		return PTR_ERR(rphy->phy_base);
	}

	/* Turn off differential receiver by default to save power */
	phy_clear_bits(rphy->phy_base + 0x0030, BIT(2));
	phy_clear_bits(rphy->phy_base + 0x0430, BIT(2));

	/* Enable pre-emphasis during non-chirp phase */
	phy_update_bits(rphy->phy_base, GENMASK(2, 0), 0x04);
	phy_update_bits(rphy->phy_base + 0x0400, GENMASK(2, 0), 0x04);

	/* Set HS eye height to 425mv(default is 400mv) */
	phy_update_bits(rphy->phy_base + 0x0030, GENMASK(6, 4), (0x05 << 4));
	phy_update_bits(rphy->phy_base + 0x0430, GENMASK(6, 4), (0x05 << 4));

	return 0;
}

static int rk3576_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	struct regmap *base = get_reg_base(rphy);
	int ret;

	if (rphy->phy_cfg->reg == 0x0) {
		/* Deassert SIDDQ to power on analog block */
		ret = regmap_write(base, 0x0010, GENMASK(29, 29) | 0x0000);
		if (ret)
			return ret;

		/* Do reset after exit IDDQ mode */
		ret = rockchip_usb2phy_reset(rphy);
		if (ret)
			return ret;

		/* HS DC Voltage Level Adjustment 4'b1001 : +5.89% */
		ret = regmap_write(base, 0x000c, GENMASK(27, 24) | 0x0900);
		if (ret)
			return ret;

		/* HS Transmitter Pre-Emphasis Current Control 2'b10 : 2x */
		ret = regmap_write(base, 0x0010, GENMASK(20, 19) | 0x0010);
		if (ret)
			return ret;
	} else if (rphy->phy_cfg->reg == 0x2000) {
		/* Deassert SIDDQ to power on analog block */
		ret = regmap_write(base, 0x2010, GENMASK(29, 29) | 0x0000);
		if (ret)
			return ret;

		/* Do reset after exit IDDQ mode */
		ret = rockchip_usb2phy_reset(rphy);
		if (ret)
			return ret;

		/* HS DC Voltage Level Adjustment 4'b1001 : +5.89% */
		ret = regmap_write(base, 0x200c, GENMASK(27, 24) | 0x0900);
		if (ret)
			return ret;

		/* HS Transmitter Pre-Emphasis Current Control 2'b10 : 2x */
		ret = regmap_write(base, 0x2010, GENMASK(20, 19) | 0x0010);
		if (ret)
			return ret;
	}

	return 0;
}

static int rk3588_usb2phy_tuning(struct rockchip_usb2phy *rphy)
{
	struct regmap *base = get_reg_base(rphy);
	int ret;

	/* Deassert SIDDQ to power on analog block */
	ret = regmap_write(base, 0x0008, GENMASK(29, 29) | 0x0000);
	if (ret)
		return ret;

	/* Do reset after exit IDDQ mode */
	ret = rockchip_usb2phy_reset(rphy);
	if (ret)
		return ret;

	/* HS DC Voltage Level Adjustment 4'b1001 : +5.89% */
	ret = regmap_write(base, 0x0004, GENMASK(27, 24) | 0x0900);
	if (ret)
		return ret;

	/* HS Transmitter Pre-Emphasis Current Control 2'b10 : 2x */
	ret = regmap_write(base, 0x0008, GENMASK(20, 19) | 0x0010);
	if (ret)
		return ret;

	return 0;
}

static struct phy_ops rockchip_usb2phy_ops = {
	.init = rockchip_usb2phy_init,
	.exit = rockchip_usb2phy_exit,
	.power_on = rockchip_usb2phy_power_on,
	.power_off = rockchip_usb2phy_power_off,
	.of_xlate = rockchip_usb2phy_of_xlate,
};

static const struct rockchip_usb2phy_cfg rk1808_phy_cfgs[] = {
	{
		.reg = 0x100,
		.num_ports	= 2,
		.clkout_ctl	= { 0x108, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0100, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0110, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x0114, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x0118, 2, 2, 0, 1 },
				.iddig_output	= { 0x0100, 10, 10, 0, 1 },
				.iddig_en	= { 0x0100, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0110, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x0114, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x0118, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x0110, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x0114, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x0118, 4, 4, 0, 1 },
				.ls_det_en	= { 0x0110, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0114, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0118, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0120, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x0120, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x0120, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0120, 5, 4, 0, 1 },
				.vbus_det_en	= { 0x001c, 15, 15, 1, 0 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x104, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x110, 1, 1, 0, 1 },
				.ls_det_st	= { 0x114, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x118, 1, 1, 0, 1 },
				.utmi_ls	= { 0x120, 17, 16, 0, 1 },
				.utmi_hstdet	= { 0x120, 19, 19, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0100, 3, 0, 5, 1 },
			.cp_det		= { 0x0120, 24, 24, 0, 1 },
			.dcp_det	= { 0x0120, 23, 23, 0, 1 },
			.dp_det		= { 0x0120, 25, 25, 0, 1 },
			.idm_sink_en	= { 0x0108, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0108, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0108, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0108, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0108, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0108, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3036_phy_cfgs[] = {
	{
		.reg = 0x17c,
		.num_ports	= 2,
		.clkout_ctl	= { 0x017c, 11, 11, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x017c, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x017c, 14, 14, 0, 1 },
				.bvalid_det_st	= { 0x017c, 15, 15, 0, 1 },
				.bvalid_det_clr	= { 0x017c, 15, 15, 0, 1 },
				.iddig_output	= { 0x017c, 10, 10, 0, 1 },
				.iddig_en	= { 0x017c, 9, 9, 0, 1 },
				.idfall_det_en  = { 0x01a0, 2, 2, 0, 1 },
				.idfall_det_st  = { 0x01a0, 3, 3, 0, 1 },
				.idfall_det_clr = { 0x01a0, 3, 3, 0, 1 },
				.idrise_det_en  = { 0x01a0, 0, 0, 0, 1 },
				.idrise_det_st  = { 0x01a0, 1, 1, 0, 1 },
				.idrise_det_clr = { 0x01a0, 1, 1, 0, 1 },
				.ls_det_en	= { 0x017c, 12, 12, 0, 1 },
				.ls_det_st	= { 0x017c, 13, 13, 0, 1 },
				.ls_det_clr	= { 0x017c, 13, 13, 0, 1 },
				.utmi_bvalid	= { 0x014c, 5, 5, 0, 1 },
				.utmi_iddig	= { 0x014c, 8, 8, 0, 1 },
				.utmi_ls	= { 0x014c, 7, 6, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0194, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0194, 14, 14, 0, 1 },
				.ls_det_st	= { 0x0194, 15, 15, 0, 1 },
				.ls_det_clr	= { 0x0194, 15, 15, 0, 1 }
			}
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk312x_phy_cfgs[] = {
	{
		.reg = 0x17c,
		.num_ports	= 2,
		.clkout_ctl	= { 0x0190, 15, 15, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x017c, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x017c, 14, 14, 0, 1 },
				.bvalid_det_st	= { 0x017c, 15, 15, 0, 1 },
				.bvalid_det_clr	= { 0x017c, 15, 15, 0, 1 },
				.iddig_output	= { 0x017c, 10, 10, 0, 1 },
				.iddig_en	= { 0x017c, 9, 9, 0, 1 },
				.idfall_det_en  = { 0x01a0, 2, 2, 0, 1 },
				.idfall_det_st  = { 0x01a0, 3, 3, 0, 1 },
				.idfall_det_clr = { 0x01a0, 3, 3, 0, 1 },
				.idrise_det_en  = { 0x01a0, 0, 0, 0, 1 },
				.idrise_det_st  = { 0x01a0, 1, 1, 0, 1 },
				.idrise_det_clr = { 0x01a0, 1, 1, 0, 1 },
				.ls_det_en	= { 0x017c, 12, 12, 0, 1 },
				.ls_det_st	= { 0x017c, 13, 13, 0, 1 },
				.ls_det_clr	= { 0x017c, 13, 13, 0, 1 },
				.utmi_bvalid	= { 0x014c, 5, 5, 0, 1 },
				.utmi_iddig	= { 0x014c, 8, 8, 0, 1 },
				.utmi_ls	= { 0x014c, 7, 6, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0194, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0194, 14, 14, 0, 1 },
				.ls_det_st	= { 0x0194, 15, 15, 0, 1 },
				.ls_det_clr	= { 0x0194, 15, 15, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x017c, 3, 0, 5, 1 },
			.cp_det		= { 0x02c0, 6, 6, 0, 1 },
			.dcp_det	= { 0x02c0, 5, 5, 0, 1 },
			.dp_det		= { 0x02c0, 7, 7, 0, 1 },
			.idm_sink_en	= { 0x0184, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0184, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0184, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0184, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0184, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0184, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk322x_phy_cfgs[] = {
	{
		.reg = 0x760,
		.num_ports	= 2,
		.phy_tuning	= rk322x_usb2phy_tuning,
		.clkout_ctl	= { 0x0768, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0760, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0680, 3, 3, 0, 1 },
				.bvalid_det_st	= { 0x0690, 3, 3, 0, 1 },
				.bvalid_det_clr	= { 0x06a0, 3, 3, 0, 1 },
				.iddig_output	= { 0x0760, 10, 10, 0, 1 },
				.iddig_en	= { 0x0760, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0680, 6, 6, 0, 1 },
				.idfall_det_st	= { 0x0690, 6, 6, 0, 1 },
				.idfall_det_clr	= { 0x06a0, 6, 6, 0, 1 },
				.idrise_det_en	= { 0x0680, 5, 5, 0, 1 },
				.idrise_det_st	= { 0x0690, 5, 5, 0, 1 },
				.idrise_det_clr	= { 0x06a0, 5, 5, 0, 1 },
				.ls_det_en	= { 0x0680, 2, 2, 0, 1 },
				.ls_det_st	= { 0x0690, 2, 2, 0, 1 },
				.ls_det_clr	= { 0x06a0, 2, 2, 0, 1 },
				.utmi_bvalid	= { 0x0480, 4, 4, 0, 1 },
				.utmi_iddig	= { 0x0480, 1, 1, 0, 1 },
				.utmi_ls	= { 0x0480, 3, 2, 0, 1 },
				.vbus_det_en	= { 0x0788, 15, 15, 1, 0 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0764, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0680, 4, 4, 0, 1 },
				.ls_det_st	= { 0x0690, 4, 4, 0, 1 },
				.ls_det_clr	= { 0x06a0, 4, 4, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0760, 3, 0, 5, 1 },
			.cp_det		= { 0x0884, 4, 4, 0, 1 },
			.dcp_det	= { 0x0884, 3, 3, 0, 1 },
			.dp_det		= { 0x0884, 5, 5, 0, 1 },
			.idm_sink_en	= { 0x0768, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0768, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0768, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0768, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0768, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0768, 11, 11, 0, 1 },
		},
	},
	{
		.reg = 0x800,
		.num_ports	= 2,
		.clkout_ctl	= { 0x0808, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x804, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0684, 1, 1, 0, 1 },
				.ls_det_st	= { 0x0694, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x06a4, 1, 1, 0, 1 }
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x800, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0684, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0694, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x06a4, 0, 0, 0, 1 }
			}
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3308_phy_cfgs[] = {
	{
		.reg = 0x100,
		.num_ports	= 2,
		.phy_tuning	= rk3308_usb2phy_tuning,
		.clkout_ctl	= { 0x0108, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0100, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x3020, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x3024, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x3028, 2, 2, 0, 1 },
				.iddig_output	= { 0x0100, 10, 10, 0, 1 },
				.iddig_en	= { 0x0100, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x3020, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x3024, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x3028, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x3020, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x3024, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x3028, 4, 4, 0, 1 },
				.ls_det_en	= { 0x3020, 0, 0, 0, 1 },
				.ls_det_st	= { 0x3024, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x3028, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0120, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x0120, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x0120, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0120, 5, 4, 0, 1 },
				.vbus_det_en	= { 0x001c, 15, 15, 1, 0 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0104, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x3020, 1, 1, 0, 1 },
				.ls_det_st	= { 0x3024, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x3028, 1, 1, 0, 1 },
				.utmi_ls	= { 0x120, 17, 16, 0, 1 },
				.utmi_hstdet	= { 0x120, 19, 19, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0100, 3, 0, 5, 1 },
			.cp_det		= { 0x0120, 24, 24, 0, 1 },
			.dcp_det	= { 0x0120, 23, 23, 0, 1 },
			.dp_det		= { 0x0120, 25, 25, 0, 1 },
			.idm_sink_en	= { 0x0108, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0108, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0108, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0108, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0108, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0108, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3328_phy_cfgs[] = {
	{
		.reg = 0x100,
		.num_ports	= 2,
		.phy_tuning = rk3328_usb2phy_tuning,
		.clkout_ctl	= { 0x108, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0100, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0110, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x0114, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x0118, 2, 2, 0, 1 },
				.iddig_output	= { 0x0100, 10, 10, 0, 1 },
				.iddig_en	= { 0x0100, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0110, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x0114, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x0118, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x0110, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x0114, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x0118, 4, 4, 0, 1 },
				.ls_det_en	= { 0x0110, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0114, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0118, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0120, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x0120, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x0120, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0120, 5, 4, 0, 1 },
				.vbus_det_en	= { 0x001c, 15, 15, 1, 0 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x104, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x110, 1, 1, 0, 1 },
				.ls_det_st	= { 0x114, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x118, 1, 1, 0, 1 },
				.utmi_ls	= { 0x120, 17, 16, 0, 1 },
				.utmi_hstdet	= { 0x120, 19, 19, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0100, 3, 0, 5, 1 },
			.cp_det		= { 0x0120, 24, 24, 0, 1 },
			.dcp_det	= { 0x0120, 23, 23, 0, 1 },
			.dp_det		= { 0x0120, 25, 25, 0, 1 },
			.idm_sink_en	= { 0x0108, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0108, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0108, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0108, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0108, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0108, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3368_phy_cfgs[] = {
	{
		.reg = 0x700,
		.num_ports	= 2,
		.clkout_ctl	= { 0x0724, 15, 15, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0700, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0680, 3, 3, 0, 1 },
				.bvalid_det_st	= { 0x0690, 3, 3, 0, 1 },
				.bvalid_det_clr = { 0x06a0, 3, 3, 0, 1 },
				.ls_det_en	= { 0x0680, 2, 2, 0, 1 },
				.ls_det_st	= { 0x0690, 2, 2, 0, 1 },
				.ls_det_clr	= { 0x06a0, 2, 2, 0, 1 },
				.utmi_bvalid	= { 0x04bc, 23, 23, 0, 1 },
				.utmi_ls	= { 0x04bc, 25, 24, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0728, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0680, 4, 4, 0, 1 },
				.ls_det_st	= { 0x0690, 4, 4, 0, 1 },
				.ls_det_clr	= { 0x06a0, 4, 4, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0700, 3, 0, 5, 1 },
			.cp_det		= { 0x04b8, 30, 30, 0, 1 },
			.dcp_det	= { 0x04b8, 29, 29, 0, 1 },
			.dp_det		= { 0x04b8, 31, 31, 0, 1 },
			.idm_sink_en	= { 0x0718, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0718, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0718, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0718, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0718, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0718, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3399_phy_cfgs[] = {
	{
		.reg		= 0xe450,
		.num_ports	= 2,
		.clkout_ctl	= { 0xe450, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus = { 0xe454, 8, 0, 0x052, 0x1d1 },
				.bvalid_det_en	= { 0xe3c0, 3, 3, 0, 1 },
				.bvalid_det_st	= { 0xe3e0, 3, 3, 0, 1 },
				.bvalid_det_clr	= { 0xe3d0, 3, 3, 0, 1 },
				.idfall_det_en	= { 0xe3c0, 5, 5, 0, 1 },
				.idfall_det_st	= { 0xe3e0, 5, 5, 0, 1 },
				.idfall_det_clr	= { 0xe3d0, 5, 5, 0, 1 },
				.idrise_det_en	= { 0xe3c0, 4, 4, 0, 1 },
				.idrise_det_st	= { 0xe3e0, 4, 4, 0, 1 },
				.idrise_det_clr	= { 0xe3d0, 4, 4, 0, 1 },
				.ls_det_en	= { 0xe3c0, 2, 2, 0, 1 },
				.ls_det_st	= { 0xe3e0, 2, 2, 0, 1 },
				.ls_det_clr	= { 0xe3d0, 2, 2, 0, 1 },
				.utmi_avalid	= { 0xe2ac, 7, 7, 0, 1 },
				.utmi_bvalid	= { 0xe2ac, 12, 12, 0, 1 },
				.utmi_iddig	= { 0xe2ac, 8, 8, 0, 1 },
				.utmi_ls	= { 0xe2ac, 14, 13, 0, 1 },
				.vbus_det_en    = { 0x449c, 15, 15, 1, 0 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0xe458, 1, 0, 0x2, 0x1 },
				.ls_det_en	= { 0xe3c0, 6, 6, 0, 1 },
				.ls_det_st	= { 0xe3e0, 6, 6, 0, 1 },
				.ls_det_clr	= { 0xe3d0, 6, 6, 0, 1 },
				.utmi_ls	= { 0xe2ac, 22, 21, 0, 1 },
				.utmi_hstdet	= { 0xe2ac, 23, 23, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0xe454, 3, 0, 5, 1 },
			.cp_det		= { 0xe2ac, 2, 2, 0, 1 },
			.dcp_det	= { 0xe2ac, 1, 1, 0, 1 },
			.dp_det		= { 0xe2ac, 0, 0, 0, 1 },
			.idm_sink_en	= { 0xe450, 8, 8, 0, 1 },
			.idp_sink_en	= { 0xe450, 7, 7, 0, 1 },
			.idp_src_en	= { 0xe450, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0xe450, 10, 10, 0, 1 },
			.vdm_src_en	= { 0xe450, 12, 12, 0, 1 },
			.vdp_src_en	= { 0xe450, 11, 11, 0, 1 },
		},
	},
	{
		.reg		= 0xe460,
		.num_ports	= 2,
		.clkout_ctl	= { 0xe460, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus = { 0xe464, 8, 0, 0x052, 0x1d1 },
				.bvalid_det_en  = { 0xe3c0, 8, 8, 0, 1 },
				.bvalid_det_st  = { 0xe3e0, 8, 8, 0, 1 },
				.bvalid_det_clr = { 0xe3d0, 8, 8, 0, 1 },
				.idfall_det_en	= { 0xe3c0, 10, 10, 0, 1 },
				.idfall_det_st	= { 0xe3e0, 10, 10, 0, 1 },
				.idfall_det_clr	= { 0xe3d0, 10, 10, 0, 1 },
				.idrise_det_en	= { 0xe3c0, 9, 9, 0, 1 },
				.idrise_det_st	= { 0xe3e0, 9, 9, 0, 1 },
				.idrise_det_clr	= { 0xe3d0, 9, 9, 0, 1 },
				.ls_det_en	= { 0xe3c0, 7, 7, 0, 1 },
				.ls_det_st	= { 0xe3e0, 7, 7, 0, 1 },
				.ls_det_clr	= { 0xe3d0, 7, 7, 0, 1 },
				.utmi_avalid	= { 0xe2ac, 10, 10, 0, 1 },
				.utmi_bvalid    = { 0xe2ac, 16, 16, 0, 1 },
				.utmi_iddig	= { 0xe2ac, 11, 11, 0, 1 },
				.utmi_ls	= { 0xe2ac, 18, 17, 0, 1 },
				.vbus_det_en    = { 0x451c, 15, 15, 1, 0 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0xe468, 1, 0, 0x2, 0x1 },
				.ls_det_en	= { 0xe3c0, 11, 11, 0, 1 },
				.ls_det_st	= { 0xe3e0, 11, 11, 0, 1 },
				.ls_det_clr	= { 0xe3d0, 11, 11, 0, 1 },
				.utmi_ls	= { 0xe2ac, 26, 25, 0, 1 },
				.utmi_hstdet	= { 0xe2ac, 27, 27, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0xe464, 3, 0, 5, 1 },
			.cp_det		= { 0xe2ac, 5, 5, 0, 1 },
			.dcp_det	= { 0xe2ac, 4, 4, 0, 1 },
			.dp_det		= { 0xe2ac, 3, 3, 0, 1 },
			.idm_sink_en	= { 0xe460, 8, 8, 0, 1 },
			.idp_sink_en	= { 0xe460, 7, 7, 0, 1 },
			.idp_src_en	= { 0xe460, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0xe460, 10, 10, 0, 1 },
			.vdm_src_en	= { 0xe460, 12, 12, 0, 1 },
			.vdp_src_en	= { 0xe460, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rv1103b_phy_cfgs[] = {
	{
		.reg = 0x20e10000,
		.num_ports	= 1,
		.phy_tuning	= rv1103b_usb2phy_tuning,
		.clkout_ctl	= { 0x50058, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x50050, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x50100, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x50104, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x50108, 2, 2, 0, 1 },
				.iddig_output	= { 0x50050, 10, 10, 0, 1 },
				.iddig_en	= { 0x50050, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x50100, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x50104, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x50108, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x50100, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x50104, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x50108, 4, 4, 0, 1 },
				.ls_det_en	= { 0x50100, 0, 0, 0, 1 },
				.ls_det_st	= { 0x50104, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x50108, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x50060, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x50060, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x50060, 6, 6, 0, 1 },
				.utmi_ls	= { 0x50060, 5, 4, 0, 1 },
			},
		},
		.chg_det = {
			.opmode		= { 0x50050, 3, 0, 5, 1 },
			.cp_det		= { 0x50060, 13, 13, 0, 1 },
			.dcp_det	= { 0x50060, 12, 12, 0, 1 },
			.dp_det		= { 0x50060, 14, 14, 0, 1 },
			.idm_sink_en	= { 0x50058, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x50058, 7, 7, 0, 1 },
			.idp_src_en	= { 0x50058, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x50058, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x50058, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x50058, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rv1106_phy_cfgs[] = {
	{
		.reg = 0xff3e0000,
		.num_ports	= 1,
		.phy_tuning	= rv1106_usb2phy_tuning,
		.clkout_ctl	= { 0x0058, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0050, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0100, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x0104, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x0108, 2, 2, 0, 1 },
				.iddig_output	= { 0x0050, 10, 10, 0, 1 },
				.iddig_en	= { 0x0050, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0100, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x0104, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x0108, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x0100, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x0104, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x0108, 4, 4, 0, 1 },
				.ls_det_en	= { 0x0100, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0104, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0108, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0060, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x0060, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x0060, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0060, 5, 4, 0, 1 },
			},
		},
		.chg_det = {
			.opmode	= { 0x0050, 3, 0, 5, 1 },
			.cp_det		= { 0x0060, 13, 13, 0, 1 },
			.dcp_det	= { 0x0060, 12, 12, 0, 1 },
			.dp_det		= { 0x0060, 14, 14, 0, 1 },
			.idm_sink_en	= { 0x0058, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0058, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0058, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0058, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0058, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0058, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rv1108_phy_cfgs[] = {
	{
		.reg = 0x100,
		.num_ports	= 2,
		.clkout_ctl	= { 0x108, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0ffa0100, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0680, 3, 3, 0, 1 },
				.bvalid_det_st	= { 0x0690, 3, 3, 0, 1 },
				.bvalid_det_clr = { 0x06a0, 3, 3, 0, 1 },
				.ls_det_en	= { 0x0680, 2, 2, 0, 1 },
				.ls_det_st	= { 0x0690, 2, 2, 0, 1 },
				.ls_det_clr	= { 0x06a0, 2, 2, 0, 1 },
				.utmi_bvalid	= { 0x0804, 10, 10, 0, 1 },
				.utmi_ls	= { 0x0804, 13, 12, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0ffa0104, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x0680, 4, 4, 0, 1 },
				.ls_det_st	= { 0x0690, 4, 4, 0, 1 },
				.ls_det_clr	= { 0x06a0, 4, 4, 0, 1 },
				.utmi_ls	= { 0x0804, 9, 8, 0, 1 },
				.utmi_hstdet	= { 0x0804, 7, 7, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0ffa0100, 3, 0, 5, 1 },
			.cp_det		= { 0x0804, 1, 1, 0, 1 },
			.dcp_det	= { 0x0804, 0, 0, 0, 1 },
			.dp_det		= { 0x0804, 2, 2, 0, 1 },
			.idm_sink_en	= { 0x0ffa0108, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0ffa0108, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0ffa0108, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0ffa0108, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0ffa0108, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0ffa0108, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3506_phy_cfgs[] = {
	{
		.reg = 0xff2b0000,
		.num_ports	= 2,
		.phy_tuning	= rk3506_usb2phy_tuning,
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0060, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0150, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x0154, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x0158, 2, 2, 0, 1 },
				.iddig_output	= { 0x0060, 10, 10, 0, 1 },
				.iddig_en	= { 0x0060, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0150, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x0154, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x0158, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x0150, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x0154, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x0158, 4, 4, 0, 1 },
				.ls_det_en	= { 0x0150, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0154, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0158, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0118, 1, 1, 0, 1 },
				.utmi_bvalid	= { 0x0118, 0, 0, 0, 1 },
				.utmi_iddig	= { 0x0118, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0118, 5, 4, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0070, 8, 0, 0x1d2, 0x1d1 },
				.ls_det_en	= { 0x0170, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0174, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0178, 0, 0, 0, 1 },
				.utmi_ls	= { 0x0118, 13, 12, 0, 1 },
				.utmi_hstdet	= { 0x0118, 15, 15, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0060, 3, 0, 5, 1 },
			.cp_det		= { 0x0118, 19, 19, 0, 1 },
			.dcp_det	= { 0x0118, 18, 18, 0, 1 },
			.dp_det		= { 0x0118, 20, 20, 0, 1 },
			.idm_sink_en	= { 0x006c, 1, 1, 0, 1 },
			.idp_sink_en	= { 0x006c, 0, 0, 0, 1 },
			.idp_src_en	= { 0x006c, 2, 2, 0, 1 },
			.rdm_pdwn_en	= { 0x006c, 3, 3, 0, 1 },
			.vdm_src_en	= { 0x006c, 5, 5, 0, 1 },
			.vdp_src_en	= { 0x006c, 4, 4, 0, 1 },
		},
	}
};

static const struct rockchip_usb2phy_cfg rk3528_phy_cfgs[] = {
	{
		.reg = 0xffdf0000,
		.num_ports	= 2,
		.phy_tuning	= rk3528_usb2phy_tuning,
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x6004c, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x60074, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x60078, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x6007c, 2, 2, 0, 1 },
				.iddig_output	= { 0x6004c, 10, 10, 0, 1 },
				.iddig_en	= { 0x6004c, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x60074, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x60078, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x6007c, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x60074, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x60078, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x6007c, 4, 4, 0, 1 },
				.ls_det_en	= { 0x60074, 0, 0, 0, 1 },
				.ls_det_st	= { 0x60078, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x6007c, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x6006c, 1, 1, 0, 1 },
				.utmi_bvalid	= { 0x6006c, 0, 0, 0, 1 },
				.utmi_iddig	= { 0x6006c, 6, 6, 0, 1 },
				.utmi_ls	= { 0x6006c, 5, 4, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x6005c, 8, 0, 0x1d2, 0x1d1 },
				.ls_det_en	= { 0x60090, 0, 0, 0, 1 },
				.ls_det_st	= { 0x60094, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x60098, 0, 0, 0, 1 },
				.utmi_ls	= { 0x6006c, 13, 12, 0, 1 },
				.utmi_hstdet	= { 0x6006c, 15, 15, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x6004c, 3, 0, 5, 1 },
			.cp_det		= { 0x6006c, 19, 19, 0, 1 },
			.dcp_det	= { 0x6006c, 18, 18, 0, 1 },
			.dp_det		= { 0x6006c, 20, 20, 0, 1 },
			.idm_sink_en	= { 0x60058, 1, 1, 0, 1 },
			.idp_sink_en	= { 0x60058, 0, 0, 0, 1 },
			.idp_src_en	= { 0x60058, 2, 2, 0, 1 },
			.rdm_pdwn_en	= { 0x60058, 3, 3, 0, 1 },
			.vdm_src_en	= { 0x60058, 5, 5, 0, 1 },
			.vdp_src_en	= { 0x60058, 4, 4, 0, 1 },
		},
	}
};

static const struct rockchip_usb2phy_cfg rk3562_phy_cfgs[] = {
	{
		.reg = 0xff740000,
		.num_ports	= 2,
		.phy_tuning	= rk3562_usb2phy_tuning,
		.clkout_ctl	= { 0x0108, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0100, 8, 0, 0, 0x1d1 },
				.bvalid_det_en	= { 0x0110, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x0114, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x0118, 2, 2, 0, 1 },
				.iddig_output	= { 0x0100, 10, 10, 0, 1 },
				.iddig_en	= { 0x0100, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0110, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x0114, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x0118, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x0110, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x0114, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x0118, 4, 4, 0, 1 },
				.ls_det_en	= { 0x0110, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0114, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0118, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0120, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x0120, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x0120, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0120, 5, 4, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0104, 8, 0, 0x1d2, 0x1d1 },
				.ls_det_en	= { 0x0110, 1, 1, 0, 1 },
				.ls_det_st	= { 0x0114, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x0118, 1, 1, 0, 1 },
				.utmi_ls	= { 0x0120, 17, 16, 0, 1 },
				.utmi_hstdet	= { 0x0120, 19, 19, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0100, 3, 0, 5, 1 },
			.cp_det		= { 0x0120, 24, 24, 0, 1 },
			.dcp_det	= { 0x0120, 23, 23, 0, 1 },
			.dp_det		= { 0x0120, 25, 25, 0, 1 },
			.idm_sink_en	= { 0x0108, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0108, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0108, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0108, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0108, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0108, 11, 11, 0, 1 },
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3568_phy_cfgs[] = {
	{
		.reg = 0xfe8a0000,
		.num_ports	= 2,
		.clkout_ctl	= { 0x0008, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0000, 8, 0, 0x052, 0x1d1 },
				.bvalid_det_en	= { 0x0080, 2, 2, 0, 1 },
				.bvalid_det_st	= { 0x0084, 2, 2, 0, 1 },
				.bvalid_det_clr = { 0x0088, 2, 2, 0, 1 },
				.iddig_output	= { 0x0000, 10, 10, 0, 1 },
				.iddig_en	= { 0x0000, 9, 9, 0, 1 },
				.idfall_det_en	= { 0x0080, 5, 5, 0, 1 },
				.idfall_det_st	= { 0x0084, 5, 5, 0, 1 },
				.idfall_det_clr = { 0x0088, 5, 5, 0, 1 },
				.idrise_det_en	= { 0x0080, 4, 4, 0, 1 },
				.idrise_det_st	= { 0x0084, 4, 4, 0, 1 },
				.idrise_det_clr = { 0x0088, 4, 4, 0, 1 },
				.ls_det_en	= { 0x0080, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0084, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0088, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x00c0, 10, 10, 0, 1 },
				.utmi_bvalid	= { 0x00c0, 9, 9, 0, 1 },
				.utmi_iddig	= { 0x00c0, 6, 6, 0, 1 },
				.utmi_ls	= { 0x00c0, 5, 4, 0, 1 },
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0004, 8, 0, 0x1d2, 0x1d1 },
				.ls_det_en	= { 0x0080, 1, 1, 0, 1 },
				.ls_det_st	= { 0x0084, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x0088, 1, 1, 0, 1 },
				.utmi_ls	= { 0x00c0, 17, 16, 0, 1 },
				.utmi_hstdet	= { 0x00c0, 19, 19, 0, 1 }
			}
		},
		.chg_det = {
			.opmode		= { 0x0000, 3, 0, 5, 1 },
			.cp_det		= { 0x00c0, 24, 24, 0, 1 },
			.dcp_det	= { 0x00c0, 23, 23, 0, 1 },
			.dp_det		= { 0x00c0, 25, 25, 0, 1 },
			.idm_sink_en	= { 0x0008, 8, 8, 0, 1 },
			.idp_sink_en	= { 0x0008, 7, 7, 0, 1 },
			.idp_src_en	= { 0x0008, 9, 9, 0, 1 },
			.rdm_pdwn_en	= { 0x0008, 10, 10, 0, 1 },
			.vdm_src_en	= { 0x0008, 12, 12, 0, 1 },
			.vdp_src_en	= { 0x0008, 11, 11, 0, 1 },
		},
	},
	{
		.reg = 0xfe8b0000,
		.num_ports	= 2,
		.clkout_ctl	= { 0x0008, 4, 4, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0000, 8, 0, 0x1d2, 0x1d1 },
				.ls_det_en	= { 0x0080, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0084, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0088, 0, 0, 0, 1 },
				.utmi_ls	= { 0x00c0, 5, 4, 0, 1 },
				.utmi_hstdet	= { 0x00c0, 7, 7, 0, 1 }
			},
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0004, 8, 0, 0x1d2, 0x1d1 },
				.ls_det_en	= { 0x0080, 1, 1, 0, 1 },
				.ls_det_st	= { 0x0084, 1, 1, 0, 1 },
				.ls_det_clr	= { 0x0088, 1, 1, 0, 1 },
				.utmi_ls	= { 0x00c0, 17, 16, 0, 1 },
				.utmi_hstdet	= { 0x00c0, 19, 19, 0, 1 }
			}
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3576_phy_cfgs[] = {
	{
		.reg = 0x0000,
		.num_ports	= 1,
		.phy_tuning	= rk3576_usb2phy_tuning,
		.clkout_ctl	= { 0x0008, 0, 0, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x0000, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x00c0, 0, 0, 0, 1 },
				.ls_det_st	= { 0x00c4, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x00c8, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x0080, 1, 1, 0, 1 },
				.utmi_bvalid	= { 0x0080, 0, 0, 0, 1 },
				.utmi_iddig	= { 0x0080, 6, 6, 0, 1 },
				.utmi_ls	= { 0x0080, 5, 4, 0, 1 },
			}
		},
		.chg_det = {
			.opmode		= { 0x0000, 8, 0, 0x055, 0x001 },
			.cp_det		= { 0x0080, 8, 8, 0, 1 },
			.dcp_det	= { 0x0080, 8, 8, 0, 1 },
			.dp_det		= { 0x0080, 9, 9, 1, 0 },
			.idm_sink_en	= { 0x0010, 5, 5, 1, 0 },
			.idp_sink_en	= { 0x0010, 5, 5, 0, 1 },
			.idp_src_en	= { 0x0010, 14, 14, 0, 1 },
			.rdm_pdwn_en	= { 0x0010, 14, 14, 0, 1 },
			.vdm_src_en	= { 0x0010, 7, 6, 0, 3 },
			.vdp_src_en	= { 0x0010, 7, 6, 0, 3 },
		},
	},
	{
		.reg = 0x2000,
		.num_ports	= 1,
		.phy_tuning	= rk3576_usb2phy_tuning,
		.clkout_ctl	= { 0x2008, 0, 0, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x2000, 8, 0, 0, 0x1d1 },
				.ls_det_en	= { 0x20c0, 0, 0, 0, 1 },
				.ls_det_st	= { 0x20c4, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x20c8, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x2080, 1, 1, 0, 1 },
				.utmi_bvalid	= { 0x2080, 0, 0, 0, 1 },
				.utmi_iddig	= { 0x2080, 6, 6, 0, 1 },
				.utmi_ls	= { 0x2080, 5, 4, 0, 1 },
			}
		},
	},
	{ /* sentinel */ }
};

static const struct rockchip_usb2phy_cfg rk3588_phy_cfgs[] = {
	{
		.reg = 0x0000,
		.num_ports	= 1,
		.phy_tuning	= rk3588_usb2phy_tuning,
		.clkout_ctl	= { 0x0000, 0, 0, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x000c, 11, 11, 0, 1 },
				.ls_det_en	= { 0x0080, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0084, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0088, 0, 0, 0, 1 },
				.utmi_avalid	= { 0x00c0, 7, 7, 0, 1 },
				.utmi_bvalid	= { 0x00c0, 6, 6, 0, 1 },
				.utmi_iddig	= { 0x00c0, 5, 5, 0, 1 },
				.utmi_ls	= { 0x00c0, 10, 9, 0, 1 },
			}
		},
		.chg_det = {
			.opmode		= { 0x0008, 2, 2, 1, 0 },
			.cp_det		= { 0x00c0, 0, 0, 0, 1 },
			.dcp_det	= { 0x00c0, 0, 0, 0, 1 },
			.dp_det		= { 0x00c0, 1, 1, 1, 0 },
			.idm_sink_en	= { 0x0008, 5, 5, 1, 0 },
			.idp_sink_en	= { 0x0008, 5, 5, 0, 1 },
			.idp_src_en	= { 0x0008, 14, 14, 0, 1 },
			.rdm_pdwn_en	= { 0x0008, 14, 14, 0, 1 },
			.vdm_src_en	= { 0x0008, 7, 6, 0, 3 },
			.vdp_src_en	= { 0x0008, 7, 6, 0, 3 },
		},
	},
	{
		.reg = 0x4000,
		.num_ports	= 1,
		.phy_tuning	= rk3588_usb2phy_tuning,
		.clkout_ctl	= { 0x0000, 0, 0, 1, 0 },
		.port_cfgs	= {
			/* Select suspend control from controller */
			[USB2PHY_PORT_OTG] = {
				.phy_sus	= { 0x000c, 11, 11, 0, 0 },
				.ls_det_en	= { 0x0080, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0084, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0088, 0, 0, 0, 1 },
				.utmi_ls	= { 0x00c0, 10, 9, 0, 1 },
			}
		},
	},
	{
		.reg = 0x8000,
		.num_ports	= 1,
		.phy_tuning	= rk3588_usb2phy_tuning,
		.clkout_ctl	= { 0x0000, 0, 0, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0008, 2, 2, 0, 1 },
				.ls_det_en	= { 0x0080, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0084, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0088, 0, 0, 0, 1 },
				.utmi_ls	= { 0x00c0, 10, 9, 0, 1 },
			}
		},
	},
	{
		.reg = 0xc000,
		.num_ports	= 1,
		.phy_tuning	= rk3588_usb2phy_tuning,
		.clkout_ctl	= { 0x0000, 0, 0, 1, 0 },
		.port_cfgs	= {
			[USB2PHY_PORT_HOST] = {
				.phy_sus	= { 0x0008, 2, 2, 0, 1 },
				.ls_det_en	= { 0x0080, 0, 0, 0, 1 },
				.ls_det_st	= { 0x0084, 0, 0, 0, 1 },
				.ls_det_clr	= { 0x0088, 0, 0, 0, 1 },
				.utmi_ls	= { 0x00c0, 10, 9, 0, 1 },
			}
		},
	},
	{ /* sentinel */ }
};

static const struct udevice_id rockchip_usb2phy_ids[] = {
#ifdef CONFIG_ROCKCHIP_PX30
	{ .compatible = "rockchip,px30-usb2phy", .data = (ulong)&rk3328_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK1808
	{ .compatible = "rockchip,rk1808-usb2phy", .data = (ulong)&rk1808_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3036
	{ .compatible = "rockchip,rk3036-usb2phy", .data = (ulong)&rk3036_phy_cfgs },
#endif
#if defined CONFIG_ROCKCHIP_RK3128 || defined CONFIG_ROCKCHIP_RK3126
	{ .compatible = "rockchip,rk3128-usb2phy", .data = (ulong)&rk312x_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK322X
	{ .compatible = "rockchip,rk322x-usb2phy", .data = (ulong)&rk322x_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3308
	{ .compatible = "rockchip,rk3308-usb2phy", .data = (ulong)&rk3308_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3328
	{ .compatible = "rockchip,rk3328-usb2phy", .data = (ulong)&rk3328_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3368
	{ .compatible = "rockchip,rk3368-usb2phy", .data = (ulong)&rk3368_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3399
	{ .compatible = "rockchip,rk3399-usb2phy", .data = (ulong)&rk3399_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3506
	{ .compatible = "rockchip,rk3506-usb2phy", .data = (ulong)&rk3506_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3528
	{ .compatible = "rockchip,rk3528-usb2phy", .data = (ulong)&rk3528_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3562
	{ .compatible = "rockchip,rk3562-usb2phy", .data = (ulong)&rk3562_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3568
	{ .compatible = "rockchip,rk3568-usb2phy", .data = (ulong)&rk3568_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3576
	{ .compatible = "rockchip,rk3576-usb2phy", .data = (ulong)&rk3576_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RK3588
	{ .compatible = "rockchip,rk3588-usb2phy", .data = (ulong)&rk3588_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RV1103B
	{ .compatible = "rockchip,rv1103b-usb2phy", .data = (ulong)&rv1103b_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RV1106
	{ .compatible = "rockchip,rv1106-usb2phy", .data = (ulong)&rv1106_phy_cfgs },
#endif
#ifdef CONFIG_ROCKCHIP_RV1108
	{ .compatible = "rockchip,rv1108-usb2phy", .data = (ulong)&rv1108_phy_cfgs },
#endif
	{ }
};

U_BOOT_DRIVER(rockchip_usb2phy_port) = {
	.name		= "rockchip_usb2phy_port",
	.id		= UCLASS_PHY,
	.ops		= &rockchip_usb2phy_ops,
};

U_BOOT_DRIVER(rockchip_usb2phy) = {
	.name		= "rockchip_usb2phy",
	.id		= UCLASS_PHY,
	.of_match	= rockchip_usb2phy_ids,
	.probe		= rockchip_usb2phy_probe,
	.bind		= rockchip_usb2phy_bind,
	.priv_auto_alloc_size = sizeof(struct rockchip_usb2phy),
};
