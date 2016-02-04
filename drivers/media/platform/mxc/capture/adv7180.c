/*
 * Copyright 2005-2014 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

/*!
 * @file adv7180.c
 *
 * @brief Analog Device ADV7180 video decoder functions
 *
 * @ingroup Camera
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-chip-ident.h>
#include "v4l2-int-device.h"
#include "mxc_v4l2_capture.h"

#define ADV7180_VOLTAGE_ANALOG               1800000
#define ADV7180_VOLTAGE_DIGITAL_CORE         1800000
#define ADV7180_VOLTAGE_DIGITAL_IO           3300000
#define ADV7180_VOLTAGE_PLL                  1800000

static int adv7180_probe(struct i2c_client *adapter,
			 const struct i2c_device_id *id);
static int adv7180_detach(struct i2c_client *client);

static const struct i2c_device_id adv7180_id[] = {
	{"adv7180", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, adv7180_id);

static struct i2c_driver adv7180_i2c_driver = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "adv7180",
		   },
	.probe = adv7180_probe,
	.remove = adv7180_detach,
	.id_table = adv7180_id,
};

/*! List of input video formats supported. The video formats is corresponding
 * with v4l2 id in video_fmt_t
 */
typedef enum {
	ADV7180_NTSC = 0,	/*!< Locked on (M) NTSC video signal. */
	ADV7180_PAL,		/*!< (B, G, H, I, N)PAL video signal. */
	ADV7180_NOT_LOCKED,	/*!< Not locked on a signal. */
} video_fmt_idx;

/*! Number of video standards supported (including 'not locked' signal). */
#define ADV7180_STD_MAX		(ADV7180_PAL + 1)

/*!
 * Maintains the information on the current state of the sensor.
 */
struct adv7180_priv {
	struct sensor_data sen;
	v4l2_std_id std_id;
	video_fmt_idx idx;
#define DVDDIO_REG	0
#define DVDD_REG	1
#define AVDD_REG	2
#define PVDD_REG	3
	struct regulator *regulators[4];
	int cvbs;
	int cea861;
	struct pinctrl *pinctrl;
	unsigned last_reg;
#define GPIO_STANDBY		0	/* 1 - powerdown */
#define GPIO_RESET		1	/* 0 - reset */
#define GPIO_CNT		2
	int	gpios[GPIO_CNT];
	enum of_gpio_flags gpio_flags[GPIO_CNT];
};


/*! Video format structure. */
typedef struct {
	int v4l2_id;		/*!< Video for linux ID. */
	char name[16];		/*!< Name (e.g., "NTSC", "PAL", etc.) */
	u16 raw_width;		/*!< Raw width. */
	u16 raw_height;		/*!< Raw height. */
	u16 active_width;	/*!< Active width. */
	u16 active_height;	/*!< Active height. */
	int frame_rate;		/*!< Frame rate. */
	u16 lines_per_field;
	u16 skip_lines;
} video_fmt_t;

/*! Description of video formats supported.
 *
 *  PAL: raw=720x625, active=720x576.
 *  NTSC: raw=720x525, active=720x480.
 */
static video_fmt_t video_fmts[] = {
	{			/*! NTSC */
	 .v4l2_id = V4L2_STD_NTSC,
	 .name = "NTSC",
	 .raw_width = 720 + 138,	/* SENS_FRM_WIDTH */
	 .raw_height = 480 + 45,	/* SENS_FRM_HEIGHT */
	 .active_width = 720,	/* ACT_FRM_WIDTH plus 1 */
	 .active_height = 480,	/* ACT_FRM_WIDTH plus 1 */
	 .frame_rate = 30,
	 .lines_per_field = 0,
	 .skip_lines = 13,
	 },
	{			/*! (B, G, H, I, N) PAL */
	 .v4l2_id = V4L2_STD_PAL,
	 .name = "PAL",
	 .raw_width = 720,
	 .raw_height = 625,
	 .active_width = 720,
	 .active_height = 576,
	 .frame_rate = 25,
	 },
	{			/*! Unlocked standard */
	 .v4l2_id = V4L2_STD_ALL,
	 .name = "Autodetect",
	 .raw_width = 720,
	 .raw_height = 625,
	 .active_width = 720,
	 .active_height = 576,
	 .frame_rate = 0,
	 },
};

/*! @brief This mutex is used to provide mutual exclusion.
 *
 *  Create a mutex that can be used to provide mutually exclusive
 *  read/write access to the globally accessible data structures
 *  and variables that were defined above.
 */
static DEFINE_MUTEX(mutex);

#define IF_NAME                    "adv7180"
#define ADV7180_INPUT_CTL              0x00	/* Input Control */
#define ADV7180_STATUS_1               0x10	/* Status #1 */
#define ADV7180_STATUS_2               0x12	/* Status #2 */
#define ADV7180_BRIGHTNESS             0x0a	/* Brightness */
#define ADV7180_IDENT                  0x11	/* IDENT */
#define ADV7180_VSYNC_FIELD_CTL_1      0x31	/* VSYNC Field Control #1 */
#define ADV7180_MANUAL_WIN_CTL         0x3d	/* Manual Window Control */
#define ADV7180_SD_SATURATION_CB       0xe3	/* SD Saturation Cb */
#define ADV7180_SD_SATURATION_CR       0xe4	/* SD Saturation Cr */
#define ADV7180_PWR_MNG                0x0f     /* Power Management */

/* supported controls */
/* This hasn't been fully implemented yet.
 * This is how it should work, though. */
static struct v4l2_queryctrl adv7180_qctrl[] = {
	{
	.id = V4L2_CID_BRIGHTNESS,
	.type = V4L2_CTRL_TYPE_INTEGER,
	.name = "Brightness",
	.minimum = 0,		/* check this value */
	.maximum = 255,		/* check this value */
	.step = 1,		/* check this value */
	.default_value = 127,	/* check this value */
	.flags = 0,
	}, {
	.id = V4L2_CID_SATURATION,
	.type = V4L2_CTRL_TYPE_INTEGER,
	.name = "Saturation",
	.minimum = 0,		/* check this value */
	.maximum = 255,		/* check this value */
	.step = 0x1,		/* check this value */
	.default_value = 127,	/* check this value */
	.flags = 0,
	}
};

const char *gpio_names[] = {
[GPIO_STANDBY] = "pwn-gpios",
[GPIO_RESET] = "rst-gpios",
};

int adv7180_gpio_state(struct adv7180_priv *adv, int index, int active)
{
	if (gpio_is_valid(adv->gpios[index])) {
		int state = (adv->gpio_flags[index] & OF_GPIO_ACTIVE_LOW) ? 1 : 0;

		state ^= active;
		gpio_set_value_cansleep(adv->gpios[index], state);
		pr_info("%s: active=%d, %s(%d)=0x%x\n", __func__, active, gpio_names[index], adv->gpios[index], state);
		return 0;
	}
	return 1;
}

static int adv7180_get_gpios(struct adv7180_priv *adv, struct device *dev)
{
	int i;
	int ret;
	enum of_gpio_flags flags;

	for (i = 0; i < GPIO_CNT; i++) {
		adv->gpios[i] = of_get_named_gpio_flags(dev->of_node, gpio_names[i], 0, &flags);
		if (!gpio_is_valid(adv->gpios[i])) {
			dev_info(dev, "%s: gpio %s not available\n", __func__, gpio_names[i]);
		} else {
			int gflags = (flags & OF_GPIO_ACTIVE_LOW) ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW;

			if ((i == GPIO_STANDBY) || (i == GPIO_RESET))
				gflags ^= GPIOF_INIT_HIGH;
			if (flags == 2)
				gflags = GPIOF_IN;

			adv->gpio_flags[i] = flags;
			dev_info(dev, "%s: %s flags(%d -> %d)\n", __func__, gpio_names[i], flags, gflags);
			ret = devm_gpio_request_one(dev, adv->gpios[i], gflags, gpio_names[i]);
			if (ret < 0) {
				pr_info("%s: request of %s failed(%d)\n", __func__, gpio_names[i], ret);
				return ret;
			}
		}
	}
	msleep(7);
	adv7180_gpio_state(adv, GPIO_RESET, 0);	//remove reset
	msleep(4);
	return 0;
}

static const char * const regulator_names[] = {
	[DVDDIO_REG] = "DOVDD",
	[DVDD_REG] = "DVDD",
	[AVDD_REG] = "AVDD",
	[PVDD_REG] = "PVDD",
};
static const unsigned reg_volt[] = {
	[DVDDIO_REG] = ADV7180_VOLTAGE_DIGITAL_IO,
	[DVDD_REG] = ADV7180_VOLTAGE_DIGITAL_CORE,
	[AVDD_REG] = ADV7180_VOLTAGE_ANALOG,
	[PVDD_REG] = ADV7180_VOLTAGE_PLL,
};

static int adv7180_regulator_enable(struct adv7180_priv *adv, struct device *dev)
{
	int ret = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(adv->regulators); i++) {
		const char *p = regulator_names[i];

		if (p) {
			adv->regulators[i] = devm_regulator_get(dev, p);
			if (!IS_ERR(adv->regulators[i])) {
				regulator_set_voltage(adv->regulators[i],
						reg_volt[i], reg_volt[i]);
				ret = regulator_enable(adv->regulators[i]);
				if (ret) {
					dev_err(dev, "set %s voltage failed\n", p);
					return ret;
				} else {
					dev_dbg(dev, "set %s voltage ok\n", p);
				}
			} else {
				dev_warn(dev, "cannot get %s voltage\n", p);
			}
		}
	}
	return ret;
}


/***********************************************************************
 * I2C transfer.
 ***********************************************************************/

/*! Read one register from a ADV7180 i2c slave device.
 *
 *  @param *reg		register in the device we wish to access.
 *
 *  @return		       0 if success, an error code otherwise.
 */
static inline int adv7180_read(struct adv7180_priv *adv, u8 reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(adv->sen.i2c_client, reg);
	if (ret < 0) {
		dev_err(&adv->sen.i2c_client->dev,
			"%s:read reg error: reg=%2x ret=%d\n", __func__, reg, ret);
	}
	return ret;
}

/*! Write one register of a ADV7180 i2c slave device.
 *
 *  @param *reg		register in the device we wish to access.
 *
 *  @return		       0 if success, an error code otherwise.
 */
static int adv7180_write_reg(struct adv7180_priv *adv, u8 reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(adv->sen.i2c_client, reg, val);
	if (ret < 0) {
		dev_err(&adv->sen.i2c_client->dev,
			"%s:write reg error:reg=%2x,val=%2x\n", __func__,
			reg, val);
	}
	return 0;
}

/***********************************************************************
 * mxc_v4l2_capture interface.
 ***********************************************************************/

static void adv7180_detect_std(struct adv7180_priv *adv, unsigned long msec)
{
	int status_1, idx;
	unsigned long orig_jiffies = jiffies;
	v4l2_std_id std_id;

	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180_get_std\n");

	while (1) {
		status_1 = adv7180_read(adv, ADV7180_STATUS_1);
		if ((status_1 & 0x35) == 5)
			break;
		if (!msec)
			break;
		if (time_after(jiffies, orig_jiffies + msecs_to_jiffies(msec))) {
			dev_err(&adv->sen.i2c_client->dev,
					"no video lock\n");
			break;
		}
		msleep(10);
	}
	if ((status_1 & 5) != 5) {
		dev_err(&adv->sen.i2c_client->dev, "status1=0x%x\n", status_1);
		return;
	}
	status_1 &= 0x70;
	mutex_lock(&mutex);
	std_id = V4L2_STD_ALL;
	idx = ADV7180_NOT_LOCKED;
	if (status_1 == 0x40) {
		std_id = V4L2_STD_PAL;
		idx = ADV7180_PAL;
	} else if (status_1 == 0) {
		std_id = V4L2_STD_NTSC;
		idx = ADV7180_NTSC;
	} else {
		dev_err(&adv->sen.i2c_client->dev,
				"Got invalid video standard(%x,%x)!\n",
				adv7180_read(adv, ADV7180_STATUS_1),
				adv7180_read(adv, ADV7180_STATUS_2));
	}

	/* This assumes autodetect which this device uses. */
	if (adv->std_id != std_id) {
		adv->std_id = std_id;
		adv->idx = idx;
		adv->sen.pix.width = video_fmts[idx].active_width;
		adv->sen.pix.height = video_fmts[idx].active_height;
		adv->sen.spix.swidth = video_fmts[idx].raw_width - 1;
		adv->sen.spix.sheight = video_fmts[idx].raw_height;
		adv->sen.spix.top = video_fmts[idx].skip_lines;
		if (adv->cea861) {
			adv->sen.spix.left = 0;
		} else {
			adv->sen.spix.swidth = video_fmts[idx].active_width;
//			adv->sen.spix.sheight = video_fmts[idx].active_height;
			adv->sen.spix.left = video_fmts[idx].lines_per_field;
		}
	}
	mutex_unlock(&mutex);
}

/*!
 * Return attributes of current video standard.
 * Since this device autodetects the current standard, this function also
 * sets the values that need to be changed if the standard changes.
 * There is no set std equivalent function.
 *
 *  @return		None.
 */
static void adv7180_get_std(struct adv7180_priv *adv, v4l2_std_id *std)
{
	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180_get_std\n");
	adv7180_detect_std(adv, (adv->std_id == V4L2_STD_ALL) ? 2500 : 0);
	*std = adv->std_id;
}

/***********************************************************************
 * IOCTL Functions from v4l2_int_ioctl_desc.
 ***********************************************************************/

/*!
 * ioctl_g_ifparm - V4L2 sensor interface handler for vidioc_int_g_ifparm_num
 * s: pointer to standard V4L2 device structure
 * p: pointer to standard V4L2 vidioc_int_g_ifparm_num ioctl structure
 *
 * Gets slave interface parameters.
 * Calculates the required xclk value to support the requested
 * clock parameters in p.  This value is returned in the p
 * parameter.
 *
 * vidioc_int_g_ifparm returns platform-specific information about the
 * interface settings used by the sensor.
 *
 * Called on open.
 */
static int ioctl_g_ifparm(struct v4l2_int_device *s, struct v4l2_ifparm *p)
{
	struct adv7180_priv *adv = s->priv;
	dev_dbg(&adv->sen.i2c_client->dev, "adv7180:ioctl_g_ifparm\n");

	if (s == NULL) {
		pr_err("   ERROR!! no slave device set!\n");
		return -1;
	}

	/* Initialize structure to 0s then set any non-0 values. */
	memset(p, 0, sizeof(*p));
	p->u.bt656.clock_curr = adv->sen.mclk;
	if (adv->cea861) {
		p->if_type = V4L2_IF_TYPE_BT656;
		p->u.bt656.mode = V4L2_IF_TYPE_BT656_MODE_NOBT_8BIT;
		p->u.bt656.nobt_hs_inv = 1;
		p->u.bt656.bt_sync_correct = 1;
	} else {
		p->if_type = V4L2_IF_TYPE_BT656_INTERLACED;
		p->u.bt656.mode = V4L2_IF_TYPE_BT656_MODE_BT_8BIT;
//		p->u.bt656.nobt_vs_inv = 1;
	}
	/* ADV7180 has a dedicated clock so no clock settings needed. */

	return 0;
}

/*!
 * Sets the camera power.
 *
 * s  pointer to the camera device
 * on if 1, power is to be turned on.  0 means power is to be turned off
 *
 * ioctl_s_power - V4L2 sensor interface handler for vidioc_int_s_power_num
 * @s: pointer to standard V4L2 device structure
 * @on: power state to which device is to be set
 *
 * Sets devices power state to requrested state, if possible.
 * This is called on open, close, suspend and resume.
 */
static int ioctl_s_power(struct v4l2_int_device *s, int on)
{
	struct adv7180_priv *adv = s->priv;

	dev_dbg(&adv->sen.i2c_client->dev, "adv7180:ioctl_s_power\n");

	if (on && !adv->sen.on) {
		if (adv7180_write_reg(adv, ADV7180_PWR_MNG, 0x04) != 0)
			return -EIO;

		/*
		 * FIXME:Additional 400ms to wait the chip to be stable?
		 * This is a workaround for preview scrolling issue.
		 */
		msleep(400);
	} else if (!on && adv->sen.on) {
		if (adv7180_write_reg(adv, ADV7180_PWR_MNG, 0x24) != 0)
			return -EIO;
	}

	adv->sen.on = on;

	return 0;
}

/*!
 * ioctl_g_parm - V4L2 sensor interface handler for VIDIOC_G_PARM ioctl
 * @s: pointer to standard V4L2 device structure
 * @a: pointer to standard V4L2 VIDIOC_G_PARM ioctl structure
 *
 * Returns the sensor's video CAPTURE parameters.
 */
static int ioctl_g_parm(struct v4l2_int_device *s, struct v4l2_streamparm *a)
{
	struct adv7180_priv *adv = s->priv;
	struct v4l2_captureparm *cparm = &a->parm.capture;

	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180:ioctl_g_parm\n");

	switch (a->type) {
	/* These are all the possible cases. */
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		pr_debug("   type is V4L2_BUF_TYPE_VIDEO_CAPTURE\n");
		memset(a, 0, sizeof(*a));
		a->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		cparm->capability = adv->sen.streamcap.capability;
		cparm->timeperframe = adv->sen.streamcap.timeperframe;
		cparm->capturemode = adv->sen.streamcap.capturemode;
		break;

	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OVERLAY:
	case V4L2_BUF_TYPE_VBI_CAPTURE:
	case V4L2_BUF_TYPE_VBI_OUTPUT:
	case V4L2_BUF_TYPE_SLICED_VBI_CAPTURE:
	case V4L2_BUF_TYPE_SLICED_VBI_OUTPUT:
		break;

	default:
		pr_debug("ioctl_g_parm:type is unknown %d\n", a->type);
		break;
	}

	return 0;
}

/*!
 * ioctl_s_parm - V4L2 sensor interface handler for VIDIOC_S_PARM ioctl
 * @s: pointer to standard V4L2 device structure
 * @a: pointer to standard V4L2 VIDIOC_S_PARM ioctl structure
 *
 * Configures the sensor to use the input parameters, if possible.  If
 * not possible, reverts to the old parameters and returns the
 * appropriate error code.
 *
 * This driver cannot change these settings.
 */
static int ioctl_s_parm(struct v4l2_int_device *s, struct v4l2_streamparm *a)
{
	struct adv7180_priv *adv = s->priv;
	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180:ioctl_s_parm\n");

	switch (a->type) {
	/* These are all the possible cases. */
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OVERLAY:
	case V4L2_BUF_TYPE_VBI_CAPTURE:
	case V4L2_BUF_TYPE_VBI_OUTPUT:
	case V4L2_BUF_TYPE_SLICED_VBI_CAPTURE:
	case V4L2_BUF_TYPE_SLICED_VBI_OUTPUT:
		break;

	default:
		pr_debug("   type is unknown - %d\n", a->type);
		break;
	}

	return 0;
}

/*!
 * ioctl_g_fmt_cap - V4L2 sensor interface handler for ioctl_g_fmt_cap
 * @s: pointer to standard V4L2 device structure
 * @f: pointer to standard V4L2 v4l2_format structure
 *
 * Returns the sensor's current pixel format in the v4l2_format
 * parameter.
 */
static int ioctl_g_fmt_cap(struct v4l2_int_device *s, struct v4l2_format *f)
{
	struct adv7180_priv *adv = s->priv;

	dev_dbg(&adv->sen.i2c_client->dev, "adv7180:ioctl_g_fmt_cap\n");

	switch (f->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		adv7180_detect_std(adv, (adv->std_id == V4L2_STD_ALL) ? 2500 : 0);
		pr_debug("   Returning size of %dx%d\n",
			 adv->sen.pix.width, adv->sen.pix.height);
		f->fmt.pix = adv->sen.pix;
		break;
	case V4L2_BUF_TYPE_SENSOR:
		f->fmt.spix = adv->sen.spix;
		break;

	case V4L2_BUF_TYPE_PRIVATE: {
		v4l2_std_id std;
		adv7180_get_std(adv, &std);
		f->fmt.pix.pixelformat = (u32)std;
		}
		break;

	default:
		f->fmt.pix = adv->sen.pix;
		break;
	}

	return 0;
}

/*!
 * ioctl_queryctrl - V4L2 sensor interface handler for VIDIOC_QUERYCTRL ioctl
 * @s: pointer to standard V4L2 device structure
 * @qc: standard V4L2 VIDIOC_QUERYCTRL ioctl structure
 *
 * If the requested control is supported, returns the control information
 * from the video_control[] array.  Otherwise, returns -EINVAL if the
 * control is not supported.
 */
static int ioctl_queryctrl(struct v4l2_int_device *s,
			   struct v4l2_queryctrl *qc)
{
	struct adv7180_priv *adv = s->priv;
	int i;

	dev_dbg(&adv->sen.i2c_client->dev, "adv7180:ioctl_queryctrl\n");

	for (i = 0; i < ARRAY_SIZE(adv7180_qctrl); i++)
		if (qc->id && qc->id == adv7180_qctrl[i].id) {
			memcpy(qc, &(adv7180_qctrl[i]),
				sizeof(*qc));
			return 0;
		}

	return -EINVAL;
}

/*!
 * ioctl_g_ctrl - V4L2 sensor interface handler for VIDIOC_G_CTRL ioctl
 * @s: pointer to standard V4L2 device structure
 * @vc: standard V4L2 VIDIOC_G_CTRL ioctl structure
 *
 * If the requested control is supported, returns the control's current
 * value from the video_control[] array.  Otherwise, returns -EINVAL
 * if the control is not supported.
 */
static int ioctl_g_ctrl(struct v4l2_int_device *s, struct v4l2_control *vc)
{
	struct adv7180_priv *adv = s->priv;
	int ret = 0;
	int sat = 0;

	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180:ioctl_g_ctrl\n");

	switch (vc->id) {
	case V4L2_CID_BRIGHTNESS:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_BRIGHTNESS\n");
		adv->sen.brightness = adv7180_read(adv, ADV7180_BRIGHTNESS);
		vc->value = adv->sen.brightness;
		break;
	case V4L2_CID_CONTRAST:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_CONTRAST\n");
		vc->value = adv->sen.contrast;
		break;
	case V4L2_CID_SATURATION:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_SATURATION\n");
		sat = adv7180_read(adv, ADV7180_SD_SATURATION_CB);
		adv->sen.saturation = sat;
		vc->value = adv->sen.saturation;
		break;
	case V4L2_CID_HUE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_HUE\n");
		vc->value = adv->sen.hue;
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_AUTO_WHITE_BALANCE\n");
		break;
	case V4L2_CID_DO_WHITE_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_DO_WHITE_BALANCE\n");
		break;
	case V4L2_CID_RED_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_RED_BALANCE\n");
		vc->value = adv->sen.red;
		break;
	case V4L2_CID_BLUE_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_BLUE_BALANCE\n");
		vc->value = adv->sen.blue;
		break;
	case V4L2_CID_GAMMA:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_GAMMA\n");
		break;
	case V4L2_CID_EXPOSURE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_EXPOSURE\n");
		vc->value = adv->sen.ae_mode;
		break;
	case V4L2_CID_AUTOGAIN:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_AUTOGAIN\n");
		break;
	case V4L2_CID_GAIN:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_GAIN\n");
		break;
	case V4L2_CID_HFLIP:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_HFLIP\n");
		break;
	case V4L2_CID_VFLIP:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_VFLIP\n");
		break;
	default:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   Default case\n");
		vc->value = 0;
		ret = -EPERM;
		break;
	}

	return ret;
}

/*!
 * ioctl_s_ctrl - V4L2 sensor interface handler for VIDIOC_S_CTRL ioctl
 * @s: pointer to standard V4L2 device structure
 * @vc: standard V4L2 VIDIOC_S_CTRL ioctl structure
 *
 * If the requested control is supported, sets the control's current
 * value in HW (and updates the video_control[] array).  Otherwise,
 * returns -EINVAL if the control is not supported.
 */
static int ioctl_s_ctrl(struct v4l2_int_device *s, struct v4l2_control *vc)
{
	struct adv7180_priv *adv = s->priv;
	int retval = 0;
	u8 tmp;

	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180:ioctl_s_ctrl\n");

	switch (vc->id) {
	case V4L2_CID_BRIGHTNESS:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_BRIGHTNESS\n");
		tmp = vc->value;
		adv7180_write_reg(adv, ADV7180_BRIGHTNESS, tmp);
		adv->sen.brightness = vc->value;
		break;
	case V4L2_CID_CONTRAST:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_CONTRAST\n");
		break;
	case V4L2_CID_SATURATION:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_SATURATION\n");
		tmp = vc->value;
		adv7180_write_reg(adv, ADV7180_SD_SATURATION_CB, tmp);
		adv7180_write_reg(adv, ADV7180_SD_SATURATION_CR, tmp);
		adv->sen.saturation = vc->value;
		break;
	case V4L2_CID_HUE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_HUE\n");
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_AUTO_WHITE_BALANCE\n");
		break;
	case V4L2_CID_DO_WHITE_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_DO_WHITE_BALANCE\n");
		break;
	case V4L2_CID_RED_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_RED_BALANCE\n");
		break;
	case V4L2_CID_BLUE_BALANCE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_BLUE_BALANCE\n");
		break;
	case V4L2_CID_GAMMA:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_GAMMA\n");
		break;
	case V4L2_CID_EXPOSURE:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_EXPOSURE\n");
		break;
	case V4L2_CID_AUTOGAIN:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_AUTOGAIN\n");
		break;
	case V4L2_CID_GAIN:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_GAIN\n");
		break;
	case V4L2_CID_HFLIP:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_HFLIP\n");
		break;
	case V4L2_CID_VFLIP:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   V4L2_CID_VFLIP\n");
		break;
	default:
		dev_dbg(&adv->sen.i2c_client->dev,
			"   Default case\n");
		retval = -EPERM;
		break;
	}

	return retval;
}

/*!
 * ioctl_enum_framesizes - V4L2 sensor interface handler for
 *			   VIDIOC_ENUM_FRAMESIZES ioctl
 * @s: pointer to standard V4L2 device structure
 * @fsize: standard V4L2 VIDIOC_ENUM_FRAMESIZES ioctl structure
 *
 * Return 0 if successful, otherwise -EINVAL.
 */
static int ioctl_enum_framesizes(struct v4l2_int_device *s,
				 struct v4l2_frmsizeenum *fsize)
{
	struct adv7180_priv *adv = s->priv;
	if (fsize->index >= 1)
		return -EINVAL;

	fsize->discrete.width = video_fmts[adv->idx].active_width;
	fsize->discrete.height  = video_fmts[adv->idx].active_height;
	return 0;
}

/*!
 * ioctl_enum_frameintervals - V4L2 sensor interface handler for
 *			       VIDIOC_ENUM_FRAMEINTERVALS ioctl
 * @s: pointer to standard V4L2 device structure
 * @fival: standard V4L2 VIDIOC_ENUM_FRAMEINTERVALS ioctl structure
 *
 * Return 0 if successful, otherwise -EINVAL.
 */
static int ioctl_enum_frameintervals(struct v4l2_int_device *s,
					 struct v4l2_frmivalenum *fival)
{
	video_fmt_t fmt;
	int i;

	if (fival->index != 0)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(video_fmts) - 1; i++) {
		fmt = video_fmts[i];
		if (fival->width  == fmt.active_width &&
		    fival->height == fmt.active_height) {
			fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
			fival->discrete.numerator = 1;
			fival->discrete.denominator = fmt.frame_rate;
			return 0;
		}
	}

	return -EINVAL;
}

/*!
 * ioctl_g_chip_ident - V4L2 sensor interface handler for
 *			VIDIOC_DBG_G_CHIP_IDENT ioctl
 * @s: pointer to standard V4L2 device structure
 * @id: pointer to int
 *
 * Return 0.
 */
static int ioctl_g_chip_ident(struct v4l2_int_device *s, int *id)
{
	((struct v4l2_dbg_chip_ident *)id)->match.type =
					V4L2_CHIP_MATCH_I2C_DRIVER;
	strcpy(((struct v4l2_dbg_chip_ident *)id)->match.name,
						"adv7180_decoder");
	((struct v4l2_dbg_chip_ident *)id)->ident = V4L2_IDENT_ADV7180;

	return 0;
}

/*!
 * ioctl_init - V4L2 sensor interface handler for VIDIOC_INT_INIT
 * @s: pointer to standard V4L2 device structure
 */
static int ioctl_init(struct v4l2_int_device *s)
{
	struct adv7180_priv *adv = s->priv;
	dev_dbg(&adv->sen.i2c_client->dev, "In adv7180:ioctl_init\n");
	return 0;
}

/*!
 * ioctl_dev_init - V4L2 sensor interface handler for vidioc_int_dev_init_num
 * @s: pointer to standard V4L2 device structure
 *
 * Initialise the device when slave attaches to the master.
 */
static int ioctl_dev_init(struct v4l2_int_device *s)
{
	struct adv7180_priv *adv = s->priv;
	dev_dbg(&adv->sen.i2c_client->dev, "adv7180:ioctl_dev_init\n");
	return 0;
}

/*!
 * This structure defines all the ioctls for this module.
 */
static struct v4l2_int_ioctl_desc adv7180_ioctl_desc[] = {

	{vidioc_int_dev_init_num, (v4l2_int_ioctl_func*)ioctl_dev_init},

	/*!
	 * Delinitialise the dev. at slave detach.
	 * The complement of ioctl_dev_init.
	 */
/*	{vidioc_int_dev_exit_num, (v4l2_int_ioctl_func *)ioctl_dev_exit}, */

	{vidioc_int_s_power_num, (v4l2_int_ioctl_func*)ioctl_s_power},
	{vidioc_int_g_ifparm_num, (v4l2_int_ioctl_func*)ioctl_g_ifparm},
/*	{vidioc_int_g_needs_reset_num,
				(v4l2_int_ioctl_func *)ioctl_g_needs_reset}, */
/*	{vidioc_int_reset_num, (v4l2_int_ioctl_func *)ioctl_reset}, */
	{vidioc_int_init_num, (v4l2_int_ioctl_func*)ioctl_init},

	/*!
	 * VIDIOC_ENUM_FMT ioctl for the CAPTURE buffer type.
	 */
/*	{vidioc_int_enum_fmt_cap_num,
				(v4l2_int_ioctl_func *)ioctl_enum_fmt_cap}, */

	/*!
	 * VIDIOC_TRY_FMT ioctl for the CAPTURE buffer type.
	 * This ioctl is used to negotiate the image capture size and
	 * pixel format without actually making it take effect.
	 */
/*	{vidioc_int_try_fmt_cap_num,
				(v4l2_int_ioctl_func *)ioctl_try_fmt_cap}, */

	{vidioc_int_g_fmt_cap_num, (v4l2_int_ioctl_func*)ioctl_g_fmt_cap},

	/*!
	 * If the requested format is supported, configures the HW to use that
	 * format, returns error code if format not supported or HW can't be
	 * correctly configured.
	 */
/*	{vidioc_int_s_fmt_cap_num, (v4l2_int_ioctl_func *)ioctl_s_fmt_cap}, */

	{vidioc_int_g_parm_num, (v4l2_int_ioctl_func*)ioctl_g_parm},
	{vidioc_int_s_parm_num, (v4l2_int_ioctl_func*)ioctl_s_parm},
	{vidioc_int_queryctrl_num, (v4l2_int_ioctl_func*)ioctl_queryctrl},
	{vidioc_int_g_ctrl_num, (v4l2_int_ioctl_func*)ioctl_g_ctrl},
	{vidioc_int_s_ctrl_num, (v4l2_int_ioctl_func*)ioctl_s_ctrl},
	{vidioc_int_enum_framesizes_num,
				(v4l2_int_ioctl_func *)ioctl_enum_framesizes},
	{vidioc_int_enum_frameintervals_num,
				(v4l2_int_ioctl_func *)
				ioctl_enum_frameintervals},
	{vidioc_int_g_chip_ident_num,
				(v4l2_int_ioctl_func *)ioctl_g_chip_ident},
};

static struct v4l2_int_slave adv7180_slave = {
	.ioctls = adv7180_ioctl_desc,
	.num_ioctls = ARRAY_SIZE(adv7180_ioctl_desc),
};

static struct v4l2_int_device adv7180_int_device = {
	.module = THIS_MODULE,
	.name = "adv7180",
	.type = v4l2_int_type_slave,
	.u = {
		.slave = &adv7180_slave,
	},
};

/* Datasheet recommends */
const unsigned char sensor_init_data[] = {
	0x01, 0xc8,
	0x02, 0x04,
	0x03, 0x00,
	0x04, 0x45,
	0x05, 0x00,
	0x06, 0x02,
	0x07, 0x7F,
	0x08, 0x80,
	0x0A, 0x00,
	0x0B, 0x00,
	0x0C, 0x36,
	0x0D, 0x7C,
	0x0E, 0x00,
	0x0F, 0x00,
	0x13, 0x00,
	0x14, 0x12,
	0x15, 0x00,
	0x16, 0x00,
	0x17, 0x01,
	0x18, 0x93,
	0xF1, 0x19,
	0x1A, 0x00,
	0x1B, 0x00,
	0x1C, 0x00,
	0x1D, 0x40,
	0x1E, 0x00,
	0x1F, 0x00,
	0x20, 0x00,
	0x21, 0x00,
	0x22, 0x00,
	0x23, 0xC0,
	0x24, 0x00,
	0x25, 0x00,
	0x26, 0x00,
	0x27, 0x58,
	0x28, 0x00,
	0x29, 0x00,
	0x2A, 0x00,
	0x2B, 0xE1,
	0x2C, 0xAE,
	0x2D, 0xF4,
	0x2E, 0x00,
	0x2F, 0xF0,
	0x30, 0x00,
	0x31, 0x12,
	0x32, 0x41,
	0x33, 0x84,
	0x34, 0x00,
	0x35, 0x02,
	0x36, 0x00,
	0x37, 0x01,
	0x38, 0x80,
	0x39, 0xC0,
	0x3A, 0x10,
	0x3B, 0x05,
	0x3C, 0x58,
	0x3D, 0xB2,
	0x3E, 0x64,
	0x3F, 0xE4,
	0x40, 0x90,
	0x41, 0x01,
	0x42, 0x7E,
	0x43, 0xA4,
	0x44, 0xFF,
	0x45, 0xB6,
	0x46, 0x12,
	0x48, 0x00,
	0x49, 0x00,
	0x4A, 0x00,
	0x4B, 0x00,
	0x4C, 0x00,
	0x4D, 0xEF,
	0x4E, 0x08,
	0x4F, 0x08,
	0x50, 0x08,
	0x51, 0x24,
	0x52, 0x0B,
	0x53, 0x4E,
	0x54, 0x80,
	0x55, 0x00,
	0x56, 0x10,
	0x57, 0x00,
	0x58, 0x00,
	0x59, 0x00,
	0x5A, 0x00,
	0x5B, 0x00,
	0x5C, 0x00,
	0x5D, 0x00,
	0x5E, 0x00,
	0x5F, 0x00,
	0x60, 0x00,
	0x61, 0x00,
	0x62, 0x20,
	0x63, 0x00,
	0x64, 0x00,
	0x65, 0x00,
	0x66, 0x00,
	0x67, 0x03,
	0x68, 0x01,
	0x69, 0x00,
	0x6A, 0x00,
	0x6B, 0xC0,
	0x6C, 0x00,
	0x6D, 0x00,
	0x6E, 0x00,
	0x6F, 0x00,
	0x70, 0x00,
	0x71, 0x00,
	0x72, 0x00,
	0x73, 0x10,
	0x74, 0x04,
	0x75, 0x01,
	0x76, 0x00,
	0x77, 0x3F,
	0x78, 0xFF,
	0x79, 0xFF,
	0x7A, 0xFF,
	0x7B, 0x1E,
	0x7C, 0xC0,
	0x7D, 0x00,
	0x7E, 0x00,
	0x7F, 0x00,
	0x80, 0x00,
	0x81, 0xC0,
	0x82, 0x04,
	0x83, 0x00,
	0x84, 0x0C,
	0x85, 0x02,
	0x86, 0x03,
	0x87, 0x63,
	0x88, 0x5A,
	0x89, 0x08,
	0x8A, 0x10,
	0x8B, 0x00,
	0x8C, 0x40,
	0x8D, 0x00,
	0x8E, 0x40,
	0x8F, 0x00,
	0x90, 0x00,
	0x91, 0x50,
	0x92, 0x00,
	0x93, 0x00,
	0x94, 0x00,
	0x95, 0x00,
	0x96, 0x00,
	0x97, 0xF0,
	0x98, 0x00,
	0x99, 0x00,
	0x9A, 0x00,
	0x9B, 0x00,
	0x9C, 0x00,
	0x9D, 0x00,
	0x9E, 0x00,
	0x9F, 0x00,
	0xA0, 0x00,
	0xA1, 0x00,
	0xA2, 0x00,
	0xA3, 0x00,
	0xA4, 0x00,
	0xA5, 0x00,
	0xA6, 0x00,
	0xA7, 0x00,
	0xA8, 0x00,
	0xA9, 0x00,
	0xAA, 0x00,
	0xAB, 0x00,
	0xAC, 0x00,
	0xAD, 0x00,
	0xAE, 0x60,
	0xAF, 0x00,
	0xB0, 0x00,
	0xB1, 0x60,
	0xB2, 0x1C,
	0xB3, 0x54,
	0xB4, 0x00,
	0xB5, 0x00,
	0xB6, 0x00,
	0xB7, 0x13,
	0xB8, 0x03,
	0xB9, 0x33,
	0xBF, 0x02,
	0xC0, 0x00,
	0xC1, 0x00,
	0xC2, 0x00,
	0xC3, 0x00,
	0xC4, 0x00,
	0xC5, 0x81,
	0xC6, 0x00,
	0xC7, 0x00,
	0xC8, 0x00,
	0xC9, 0x04,
	0xCC, 0x69,
	0xCD, 0x00,
	0xCE, 0x01,
	0xCF, 0xB4,
	0xD0, 0x00,
	0xD1, 0x10,
	0xD2, 0xFF,
	0xD3, 0xFF,
	0xD4, 0x7F,
	0xD5, 0x7F,
	0xD6, 0x3E,
	0xD7, 0x08,
	0xD8, 0x3C,
	0xD9, 0x08,
	0xDA, 0x3C,
	0xDB, 0x9B,
	0xDC, 0xAC,
	0xDD, 0x4C,
	0xDE, 0x00,
	0xDF, 0x00,
	0xE0, 0x14,
	0xE1, 0x80,
	0xE2, 0x80,
	0xE3, 0x80,
	0xE4, 0x80,
	0xE5, 0x25,
	0xE6, 0x44,
	0xE7, 0x63,
	0xE8, 0x65,
	0xE9, 0x14,
	0xEA, 0x63,
	0xEB, 0x55,
	0xEC, 0x55,
	0xEE, 0x00,
	0xEF, 0x4A,
	0xF0, 0x44,
	0xF1, 0x0C,
	0xF2, 0x32,
	0xF3, 0x00,
	0xF4, 0x3F,
	0xF5, 0xE0,
	0xF6, 0x69,
	0xF7, 0x10,
	0xF8, 0x00,
	0xF9, 0x03,
	0xFA, 0xFA,
	0xFB, 0x40,
};
/***********************************************************************
 * I2C client and driver.
 ***********************************************************************/

/*! ADV7180 Reset function.
 *
 *  @return		None.
 */
static void adv7180_hard_reset(struct adv7180_priv *adv)
{
	int i;
	const unsigned char *p;

	dev_dbg(&adv->sen.i2c_client->dev,
		"In adv7180:adv7180_hard_reset\n");

	if (adv->cvbs) {
#if 1
		/* Set CVBS input on AIN1 */
		adv7180_write_reg(adv, ADV7180_INPUT_CTL, 0x00);
#else
		/* Set CVBS input on AIN2 */
		adv7180_write_reg(adv, ADV7180_INPUT_CTL, 0x03);
#endif
	} else {
		/*
		 * Set YPbPr input on AIN1,4,5 and normal
		 * operations(autodection of all stds).
		 */
		adv7180_write_reg(adv, ADV7180_INPUT_CTL, 0x09);
	}
	for (p = sensor_init_data, i = 0; i < ARRAY_SIZE(sensor_init_data); i += 2, p += 2) {
		adv7180_write_reg(adv, p[0], p[1]);
	}
}


static ssize_t show_reg(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct adv7180_priv *adv = adv7180_int_device.priv;
	s32 rval = adv7180_read(adv, adv->last_reg);

	if (rval < 0)
		return rval;
	return sprintf(buf, "0x%02x", rval);
}

static ssize_t store_reg(struct device *dev, struct device_attribute *attr,
	 const char *buf, size_t count)
{
	struct adv7180_priv *adv = adv7180_int_device.priv;
	unsigned reg, value;
	int numscanned = sscanf(buf,"%x %x\n", &reg, &value);

	if (reg > 0xff) {
		dev_err(dev,"%s: invalid register: use form 0xREG [0xVAL]\n",
			__func__);
		return count;
	}
	if (numscanned == 2) {
		int rval = adv7180_write_reg(adv, reg, value);
		if (rval)
			dev_err(dev, "%s: error %d setting reg 0x%04x to 0x%02x\n",
				__func__, rval, reg, value);
	} else if (numscanned == 1) {
		adv->last_reg = reg;
	} else {
		dev_err(dev,"%s: invalid register: use form 0xREG [0xVAL]\n",
			__func__);
	}
	return count;
}
static DEVICE_ATTR(adv7180_reg, S_IRWXUGO, show_reg, store_reg);

/*! ADV7180 I2C attach function.
 *
 *  @param *adapter	struct i2c_adapter *.
 *
 *  @return		Error code indicating success or failure.
 */

/*!
 * ADV7180 I2C probe function.
 * Function set in i2c_driver struct.
 * Called by insmod.
 *
 *  @param *adapter	I2C adapter descriptor.
 *
 *  @return		Error code indicating success or failure.
 */
static int adv7180_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct adv7180_priv *adv;
	int rev_id;
	int ret = 0;
	struct pinctrl_state *pins;
	struct pinctrl *pinctrl;
	struct device *dev = &client->dev;
	video_fmt_idx idx = ADV7180_NOT_LOCKED;

	pr_debug("%s\n", __func__);

	/* ov5640 pinctrl */
	pinctrl = devm_pinctrl_get_select_default(dev);
	if (IS_ERR(pinctrl)) {
		dev_err(dev, "setup pinctrl failed\n");
		return PTR_ERR(pinctrl);
	}

	adv = kzalloc(sizeof(struct adv7180_priv), GFP_KERNEL);
	if (!adv)
		return -ENOMEM;

	ret = adv7180_get_gpios(adv, dev);
	if (ret)
		goto exit1;

	ret = adv7180_regulator_enable(adv, dev);
	if (ret < 0)
		goto exit1;

	ret = of_property_read_u32(dev->of_node, "cea861",
					&adv->cea861);
	if (ret) {
		dev_err(dev, "cea861 missing or invalid\n");
		goto exit1;
	}
	pr_info("%s: cea861=%d\n", __func__, adv->cea861);

	adv->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(adv->pinctrl)) {
		ret = PTR_ERR(adv->pinctrl);
		goto exit1;
	}


	pins = pinctrl_lookup_state(adv->pinctrl, adv->cea861 ? "cea861" : "no_cea861");
	if (IS_ERR(pins)) {
		ret = PTR_ERR(pins);
		goto exit1;
	}

	ret = pinctrl_select_state(adv->pinctrl, pins);
	if (ret)
		goto exit1;

	if (!adv7180_gpio_state(adv, GPIO_STANDBY, 0))
		msleep(2);

	/* Set initial values for the sensor struct. */
	adv->sen.i2c_client = client;
	adv->sen.streamcap.timeperframe.denominator = 30;
	adv->sen.streamcap.timeperframe.numerator = 1;
	adv->std_id = V4L2_STD_ALL;
	adv->idx = idx;
	adv->sen.pix.width = video_fmts[idx].active_width;
	adv->sen.pix.height = video_fmts[idx].active_height;
	adv->sen.spix.swidth = video_fmts[idx].raw_width - 1;
	adv->sen.spix.sheight = video_fmts[idx].raw_height;
	adv->sen.spix.top = video_fmts[idx].skip_lines;
	if (adv->cea861) {
		adv->sen.spix.left = 0;
	} else {
		adv->sen.spix.swidth = video_fmts[idx].active_width;
//		adv->sen.spix.sheight = video_fmts[idx].active_height;
		adv->sen.spix.left = video_fmts[idx].lines_per_field;
	}
	adv->sen.pix.pixelformat = V4L2_PIX_FMT_UYVY;  /* YUV422 */
	adv->sen.pix.priv = 1;  /* 1 is used to indicate TV in */
	adv->sen.on = true;

	adv->sen.sensor_clk = devm_clk_get(dev, "csi_mclk");
	if (IS_ERR(adv->sen.sensor_clk)) {
		dev_info(dev, "get mclk failed\n");
	}

	ret = of_property_read_u32(dev->of_node, "mclk",
					&adv->sen.mclk);
	if (ret) {
		dev_err(dev, "mclk frequency is invalid\n");
		goto exit1;
	}

	ret = of_property_read_u32(
		dev->of_node, "mclk_source",
		(u32 *) &(adv->sen.mclk_source));
	if (ret) {
		dev_err(dev, "mclk_source invalid\n");
		goto exit1;
	}

	ret = of_property_read_u32(dev->of_node, "ipu_id",
					&adv->sen.ipu_id);
	if (ret) {
		dev_err(dev, "ipu_id missing or invalid\n");
		goto exit1;
	}

	ret = of_property_read_u32(dev->of_node, "csi_id",
					&(adv->sen.csi));
	if (ret) {
		dev_err(dev, "csi_id invalid\n");
		goto exit1;
	}

	if (!IS_ERR(adv->sen.sensor_clk))
		clk_prepare_enable(adv->sen.sensor_clk);

	dev_dbg(&adv->sen.i2c_client->dev,
		"%s:adv7180 probe i2c address is 0x%02X\n",
		__func__, adv->sen.i2c_client->addr);

	/*! Read the revision ID of the tvin chip */
	rev_id = adv7180_read(adv, ADV7180_IDENT);
	if (rev_id < 0) {
		ret = rev_id;
		goto exit1;
	}
	dev_dbg(dev,
		"%s:Analog Device adv7%2X0 detected!\n", __func__,
		rev_id);

	ret = of_property_read_u32(dev->of_node, "cvbs", &adv->cvbs);
	if (ret) {
		dev_err(dev, "cvbs setting is not found\n");
		adv->cvbs = true;
	}

	/*! ADV7180 initialization. */
	adv7180_hard_reset(adv);

	pr_debug("   type is %d (expect %d)\n",
		 adv7180_int_device.type, v4l2_int_type_slave);
	pr_debug("   num ioctls is %d\n",
		 adv7180_int_device.u.slave->num_ioctls);

	/* This function attaches this structure to the /dev/video0 device.
	 * The pointer in priv points to the adv7180_priv structure here.*/
	adv7180_int_device.priv = adv;
	i2c_set_clientdata(client, adv);
	ret = v4l2_int_device_register(&adv7180_int_device);

	if (!IS_ERR(adv->sen.sensor_clk))
		clk_disable_unprepare(adv->sen.sensor_clk);

	if (device_create_file(dev, &dev_attr_adv7180_reg))
		dev_err(dev, "Error on creating sysfs file for adv7180_reg\n");
	else
		dev_err(dev, "created sysfs entry for reading regs\n");
	return ret;

exit1:
	kfree(adv);
	return ret;
}

/*!
 * ADV7180 I2C detach function.
 * Called on rmmod.
 *
 *  @param *client	struct i2c_client*.
 *
 *  @return		Error code indicating success or failure.
 */
static int adv7180_detach(struct i2c_client *client)
{
	struct adv7180_priv *adv = i2c_get_clientdata(client);
	int i;

	dev_dbg(&adv->sen.i2c_client->dev,
		"%s:Removing %s video decoder @ 0x%02X from adapter %s\n",
		__func__, IF_NAME, client->addr << 1, client->adapter->name);

	device_remove_file(&client->dev, &dev_attr_adv7180_reg);
	/* Power down via i2c */
	adv7180_write_reg(adv, ADV7180_PWR_MNG, 0x24);

	for (i = 0; i < ARRAY_SIZE(adv->regulators); i++) {
		struct regulator *reg = adv->regulators[i];

		if (reg)
			regulator_disable(reg);
	}

	v4l2_int_device_unregister(&adv7180_int_device);
	kfree(adv);
	return 0;
}

/*!
 * ADV7180 init function.
 * Called on insmod.
 *
 * @return    Error code indicating success or failure.
 */
static __init int adv7180_init(void)
{
	u8 err = 0;

	pr_debug("In adv7180_init\n");

	/* Tells the i2c driver what functions to call for this driver. */
	err = i2c_add_driver(&adv7180_i2c_driver);
	if (err != 0)
		pr_err("%s:driver registration failed, error=%d\n",
			__func__, err);

	return err;
}

/*!
 * ADV7180 cleanup function.
 * Called on rmmod.
 *
 * @return   Error code indicating success or failure.
 */
static void __exit adv7180_clean(void)
{
	pr_debug("In adv7180_clean\n");
	i2c_del_driver(&adv7180_i2c_driver);
}

module_init(adv7180_init);
module_exit(adv7180_clean);

MODULE_AUTHOR("Freescale Semiconductor");
MODULE_DESCRIPTION("Anolog Device ADV7180 video decoder driver");
MODULE_LICENSE("GPL");
