/*
 * wm831x-core.c  --  Device access for Wolfson WM831x PMICs
 *
 * Copyright 2009 Wolfson Microelectronics PLC.
 *
 * Author: Mark Brown <broonie@opensource.wolfsonmicro.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bcd.h>
#include <linux/delay.h>
#include <linux/mfd/core.h>
#include <linux/slab.h>

#include <linux/i2c.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
//#include <linux/gpio.h>

#include <linux/mfd/wm831x/core.h>
#include <linux/mfd/wm831x/pdata.h>
#include <linux/mfd/wm831x/irq.h>
#include <linux/mfd/wm831x/auxadc.h>
#include <linux/mfd/wm831x/otp.h>
#include <linux/mfd/wm831x/regulator.h>

#define WM831X_RTC_ALM_ENA                      0x0400  /* RTC_ALM_ENA */

extern void acquire_wake_up_wake_lock(void);

static struct wm831x *the_wm831x;

/* Current settings - values are 2*2^(reg_val/4) microamps.  These are
 * exported since they are used by multiple drivers.
 */
int wm831x_isinkv_values[WM831X_ISINK_MAX_ISEL + 1] = {
	2,
	2,
	3,
	3,
	4,
	5,
	6,
	7,
	8,
	10,
	11,
	13,
	16,
	19,
	23,
	27,
	32,
	38,
	45,
	54,
	64,
	76,
	91,
	108,
	128,
	152,
	181,
	215,
	256,
	304,
	362,
	431,
	512,
	609,
	724,
	861,
	1024,
	1218,
	1448,
	1722,
	2048,
	2435,
	2896,
	3444,
	4096,
	4871,
	5793,
	6889,
	8192,
	9742,
	11585,
	13777,
	16384,
	19484,
	23170,
	27554,
};
EXPORT_SYMBOL_GPL(wm831x_isinkv_values);

static int wm831x_reg_locked(struct wm831x *wm831x, unsigned short reg)
{
	if (!wm831x->locked)
		return 0;

	switch (reg) {
	case WM831X_WATCHDOG:
	//case WM831X_DC4_CONTROL:
	//case WM831X_ON_PIN_CONTROL:
	case WM831X_BACKUP_CHARGER_CONTROL:
	case WM831X_CHARGER_CONTROL_1:
	case WM831X_CHARGER_CONTROL_2:
		return 1;

	default:
		return 0;
	}
}

#ifdef CONFIG_DEBUG_FS
static int wm831x_regfile_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t wm831x_regfile_write(struct file *file, const char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct wm831x *wm831x = file->private_data;
	char buf[64], *p, *q;
	ssize_t bufsize;
	unsigned long user_reg, user_value;
	int ret;
	unsigned short reg, value;

	if (*ppos < 0 || !count)
		return -EINVAL;

	bufsize = min(count, sizeof buf - 1);
	if (copy_from_user(buf, user_buf, bufsize))
		return -EFAULT;
	buf[bufsize] = '\0';

	p = buf;
	while (p < &buf[sizeof buf] && *p == ' ')
		++p;
	if (p == &buf[sizeof buf])
		return -EINVAL;

	if (*p == 'r') {
		/* read command */
		++p;
		while (p < &buf[sizeof buf] && *p == ' ')
			++p;
		if (p == &buf[sizeof buf])
			return -EINVAL;

		ret = strict_strtoul(p, 16, &user_reg);
		if (ret)
			return ret;
		reg = user_reg;

		value = wm831x_reg_read(wm831x, reg);
		if (value < 0)
			return value;

		dev_err(wm831x->dev, "read: reg[%#02x]: %#02x\n",
			reg, value);
	} else if (*p == 'w') {
		/* write command */
		++p;
		while (p < &buf[sizeof buf] && *p == ' ')
			++p;
		if (p == &buf[sizeof buf])
			return -EINVAL;

		q = p;
		while (q < &buf[sizeof buf] && *q != ' ')
			++q;
		if (q == &buf[sizeof buf])
			return -EINVAL;
		*q = '\0';

		ret = strict_strtoul(p, 16, &user_reg);
		if (ret)
			return ret;
		reg = user_reg;

		*q = ' ';
		while (q < &buf[sizeof buf] && *q == ' ')
			++q;
		if (q == &buf[sizeof buf])
			return -EINVAL;

		ret = strict_strtoul(q, 16, &user_value);
		if (ret)
			return ret;
		value = user_value;

		if (wm831x_reg_locked(wm831x, reg))
			wm831x_reg_lock(wm831x);
		ret = wm831x_reg_write(wm831x, reg, value);
		if (wm831x_reg_locked(wm831x, reg))
			wm831x_reg_unlock(wm831x);
		if (ret < 0)
			return ret;

		dev_err(wm831x->dev,
			"write: reg[%#02x]: %#02x\n",
			reg, value);
	} else {
		return -EINVAL;
	}

	return bufsize;
}

static const struct file_operations wm831x_regfile_fops = {
	.open = wm831x_regfile_open,
	.write = wm831x_regfile_write
};

static struct dentry *wm831x_debugfs_root;
static struct dentry *wm831x_regfile;

static void wm831x_init_debugfs(struct wm831x *wm831x)
{
	struct i2c_client *i2c = container_of(wm831x->dev,
					      struct i2c_client, dev);

	wm831x_debugfs_root = debugfs_create_dir(i2c->name, NULL);
	if (!wm831x_debugfs_root) {
		dev_err(wm831x->dev, "Failed to create debugfs directory\n");
		return;
	}

	wm831x_regfile =  debugfs_create_file("regfile", 0644,
					      wm831x_debugfs_root,
					      wm831x,
					      &wm831x_regfile_fops);
	if (!wm831x_regfile) {
		dev_err(wm831x->dev, "Failed to create debugfs regfile entry\n");
		goto exit_debugfs_root;
	}

	return;

exit_debugfs_root:
	debugfs_remove(wm831x_debugfs_root);
}
#endif

/**
 * wm831x_reg_unlock: Unlock user keyed registers
 *
 * The WM831x has a user key preventing writes to particularly
 * critical registers.  This function locks those registers,
 * allowing writes to them.
 */
void wm831x_reg_lock(struct wm831x *wm831x)
{
	int ret;

	ret = wm831x_reg_write(wm831x, WM831X_SECURITY_KEY, 0);
	if (ret == 0) {
		dev_vdbg(wm831x->dev, "Registers locked\n");

		mutex_lock(&wm831x->io_lock);
		WARN_ON(wm831x->locked);
		wm831x->locked = 1;
		mutex_unlock(&wm831x->io_lock);
	} else {
		dev_err(wm831x->dev, "Failed to lock registers: %d\n", ret);
	}

}
EXPORT_SYMBOL_GPL(wm831x_reg_lock);

/**
 * wm831x_reg_unlock: Unlock user keyed registers
 *
 * The WM831x has a user key preventing writes to particularly
 * critical registers.  This function locks those registers,
 * preventing spurious writes.
 */
int wm831x_reg_unlock(struct wm831x *wm831x)
{
	int ret;

	/* 0x9716 is the value required to unlock the registers */
	ret = wm831x_reg_write(wm831x, WM831X_SECURITY_KEY, 0x9716);
	if (ret == 0) {
		dev_vdbg(wm831x->dev, "Registers unlocked\n");

		mutex_lock(&wm831x->io_lock);
		WARN_ON(!wm831x->locked);
		wm831x->locked = 0;
		mutex_unlock(&wm831x->io_lock);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(wm831x_reg_unlock);

static int wm831x_read(struct wm831x *wm831x, unsigned short reg,
		       int bytes, void *dest)
{
	int ret, i;
	u16 *buf = dest;
	u16 retry_cnt = 0;

	BUG_ON(bytes % 2);
	BUG_ON(bytes <= 0);

retry:
	ret = wm831x->read_dev(wm831x, reg, bytes, dest);
	if (ret < 0){
		if(retry_cnt < 200){
			udelay(200);
			retry_cnt++;
			goto retry;
		}
		return ret;
	}

	for (i = 0; i < bytes / 2; i++) {
		buf[i] = be16_to_cpu(buf[i]);

		dev_vdbg(wm831x->dev, "Read %04x from R%d(0x%x)\n",
			 buf[i], reg + i, reg + i);
	}

	return 0;
}

/**
 * wm831x_reg_read: Read a single WM831x register.
 *
 * @wm831x: Device to read from.
 * @reg: Register to read.
 */
int wm831x_reg_read(struct wm831x *wm831x, unsigned short reg)
{
	unsigned short val;
	int ret;

	mutex_lock(&wm831x->io_lock);

	ret = wm831x_read(wm831x, reg, 2, &val);

	mutex_unlock(&wm831x->io_lock);

	if (ret < 0)
		return ret;
	else
		return val;
}
EXPORT_SYMBOL_GPL(wm831x_reg_read);

/**
 * wm831x_bulk_read: Read multiple WM831x registers
 *
 * @wm831x: Device to read from
 * @reg: First register
 * @count: Number of registers
 * @buf: Buffer to fill.
 */
int wm831x_bulk_read(struct wm831x *wm831x, unsigned short reg,
		     int count, u16 *buf)
{
	int ret;

	mutex_lock(&wm831x->io_lock);

	ret = wm831x_read(wm831x, reg, count * 2, buf);

	mutex_unlock(&wm831x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(wm831x_bulk_read);

static int wm831x_write(struct wm831x *wm831x, unsigned short reg,
			int bytes, void *src)
{
	u16 *buf = src;
	int i;
	int retry_cnt = 0;
	int ret;

	BUG_ON(bytes % 2);
	BUG_ON(bytes <= 0);

	for (i = 0; i < bytes / 2; i++) {
		if (wm831x_reg_locked(wm831x, reg))
			return -EPERM;

		dev_vdbg(wm831x->dev, "Write %04x to R%d(0x%x)\n",
			 buf[i], reg + i, reg + i);

		buf[i] = cpu_to_be16(buf[i]);
	}

retry:
	ret = wm831x->write_dev(wm831x, reg, bytes, src);
	if (ret < 0) {
		if (retry_cnt < 200) {
			udelay(200);
			retry_cnt++;
			goto retry;
		}
	}

	return ret;
	
}

/**
 * wm831x_reg_write: Write a single WM831x register.
 *
 * @wm831x: Device to write to.
 * @reg: Register to write to.
 * @val: Value to write.
 */
int wm831x_reg_write(struct wm831x *wm831x, unsigned short reg,
		     unsigned short val)
{
	int ret;

	mutex_lock(&wm831x->io_lock);

	ret = wm831x_write(wm831x, reg, 2, &val);

	mutex_unlock(&wm831x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(wm831x_reg_write);

/**
 * wm831x_set_bits: Set the value of a bitfield in a WM831x register
 *
 * @wm831x: Device to write to.
 * @reg: Register to write to.
 * @mask: Mask of bits to set.
 * @val: Value to set (unshifted)
 */
int wm831x_set_bits(struct wm831x *wm831x, unsigned short reg,
		    unsigned short mask, unsigned short val)
{
	int ret;
	u16 r;

	mutex_lock(&wm831x->io_lock);

	ret = wm831x_read(wm831x, reg, 2, &r);
	if (ret < 0)
		goto out;

	r &= ~mask;
	r |= val;

	ret = wm831x_write(wm831x, reg, 2, &r);

out:
	mutex_unlock(&wm831x->io_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(wm831x_set_bits);

/**
 * wm831x_auxadc_read: Read a value from the WM831x AUXADC
 *
 * @wm831x: Device to read from.
 * @input: AUXADC input to read.
 */
int wm831x_auxadc_read(struct wm831x *wm831x, enum wm831x_auxadc input)
{
	int ret, src, irq_masked, timeout;

	/* Are we using the interrupt? */
	irq_masked = wm831x_reg_read(wm831x, WM831X_INTERRUPT_STATUS_1_MASK);
	irq_masked &= WM831X_AUXADC_DATA_EINT;

	mutex_lock(&wm831x->auxadc_lock);

	ret = wm831x_set_bits(wm831x, WM831X_AUXADC_CONTROL,
			      WM831X_AUX_ENA, WM831X_AUX_ENA);
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to enable AUXADC: %d\n", ret);
		goto out;
	}

	/* We force a single source at present */
	src = input;
	ret = wm831x_reg_write(wm831x, WM831X_AUXADC_SOURCE,
			       1 << src);
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to set AUXADC source: %d\n", ret);
		goto out;
	}

	/* Clear any notification from a very late arriving interrupt */
	try_wait_for_completion(&wm831x->auxadc_done);

	ret = wm831x_set_bits(wm831x, WM831X_AUXADC_CONTROL,
			      WM831X_AUX_CVT_ENA, WM831X_AUX_CVT_ENA);
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to start AUXADC: %d\n", ret);
		goto disable;
	}

	if (irq_masked) {
		/* If we're not using interrupts then poll the
		 * interrupt status register */
		timeout = 5;
		while (timeout) {
			msleep(1);

			ret = wm831x_reg_read(wm831x,
					      WM831X_INTERRUPT_STATUS_1);
			if (ret < 0) {
				dev_err(wm831x->dev,
					"ISR 1 read failed: %d\n", ret);
				goto disable;
			}

			/* Did it complete? */
			if (ret & WM831X_AUXADC_DATA_EINT) {
				wm831x_reg_write(wm831x,
						 WM831X_INTERRUPT_STATUS_1,
						 WM831X_AUXADC_DATA_EINT);
				break;
			} else {
				dev_err(wm831x->dev,
					"AUXADC conversion timeout\n");
				ret = -EBUSY;
				goto disable;
			}
		}
	} else {
		/* If we are using interrupts then wait for the
		 * interrupt to complete.  Use an extremely long
		 * timeout to handle situations with heavy load where
		 * the notification of the interrupt may be delayed by
		 * threaded IRQ handling. */
		if (!wait_for_completion_timeout(&wm831x->auxadc_done,
						 msecs_to_jiffies(500))) {
			dev_err(wm831x->dev, "Timed out waiting for AUXADC\n");
			ret = -EBUSY;
			goto disable;
		}
	}

	ret = wm831x_reg_read(wm831x, WM831X_AUXADC_DATA);
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to read AUXADC data: %d\n", ret);
	} else {
		src = ((ret & WM831X_AUX_DATA_SRC_MASK)
		       >> WM831X_AUX_DATA_SRC_SHIFT) - 1;

		if (src == 14)
			src = WM831X_AUX_CAL;

		if (src != input) {
			dev_err(wm831x->dev, "Data from source %d not %d\n",
				src, input);
			ret = -EINVAL;
		} else {
			ret &= WM831X_AUX_DATA_MASK;
		}
	}

disable:
	wm831x_set_bits(wm831x, WM831X_AUXADC_CONTROL, WM831X_AUX_ENA, 0);
out:
	mutex_unlock(&wm831x->auxadc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(wm831x_auxadc_read);

static irqreturn_t wm831x_auxadc_irq(int irq, void *irq_data)
{
	struct wm831x *wm831x = irq_data;

	complete(&wm831x->auxadc_done);

	return IRQ_HANDLED;
}

static irqreturn_t wake_request_from_sub_cpu(int irq, void *irq_data)
{
	//printk("%s\n", __func__);
	acquire_wake_up_wake_lock();
	return IRQ_HANDLED;
}

/**
 * wm831x_auxadc_read_uv: Read a voltage from the WM831x AUXADC
 *
 * @wm831x: Device to read from.
 * @input: AUXADC input to read.
 */
int wm831x_auxadc_read_uv(struct wm831x *wm831x, enum wm831x_auxadc input)
{
	int ret;

	ret = wm831x_auxadc_read(wm831x, input);
	if (ret < 0)
		return ret;

	ret *= 1465;

	return ret;
}
EXPORT_SYMBOL_GPL(wm831x_auxadc_read_uv);

static struct resource wm831x_dcdc1_resources[] = {
	{
		.start = WM831X_DC1_CONTROL_1,
		.end   = WM831X_DC1_DVS_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_DC1,
		.end   = WM831X_IRQ_UV_DC1,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name  = "HC",
		.start = WM831X_IRQ_HC_DC1,
		.end   = WM831X_IRQ_HC_DC1,
		.flags = IORESOURCE_IRQ,
	},
};


static struct resource wm831x_dcdc2_resources[] = {
	{
		.start = WM831X_DC2_CONTROL_1,
		.end   = WM831X_DC2_DVS_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_DC2,
		.end   = WM831X_IRQ_UV_DC2,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name  = "HC",
		.start = WM831X_IRQ_HC_DC2,
		.end   = WM831X_IRQ_HC_DC2,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_dcdc3_resources[] = {
	{
		.start = WM831X_DC3_CONTROL_1,
		.end   = WM831X_DC3_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_DC3,
		.end   = WM831X_IRQ_UV_DC3,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_dcdc4_resources[] = {
	{
		.start = WM831X_DC4_CONTROL,
		.end   = WM831X_DC4_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_DC4,
		.end   = WM831X_IRQ_UV_DC4,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm8320_dcdc4_buck_resources[] = {
	{
		.start = WM831X_DC4_CONTROL,
		.end   = WM832X_DC4_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_DC4,
		.end   = WM831X_IRQ_UV_DC4,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_gpio_resources[] = {
	{
		.start = WM831X_IRQ_GPIO_1,
		.end   = WM831X_IRQ_GPIO_16,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_isink1_resources[] = {
	{
		.start = WM831X_CURRENT_SINK_1,
		.end   = WM831X_CURRENT_SINK_1,
		.flags = IORESOURCE_IO,
	},
	{
		.start = WM831X_IRQ_CS1,
		.end   = WM831X_IRQ_CS1,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_isink2_resources[] = {
	{
		.start = WM831X_CURRENT_SINK_2,
		.end   = WM831X_CURRENT_SINK_2,
		.flags = IORESOURCE_IO,
	},
	{
		.start = WM831X_IRQ_CS2,
		.end   = WM831X_IRQ_CS2,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo1_resources[] = {
	{
		.start = WM831X_LDO1_CONTROL,
		.end   = WM831X_LDO1_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO1,
		.end   = WM831X_IRQ_UV_LDO1,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo2_resources[] = {
	{
		.start = WM831X_LDO2_CONTROL,
		.end   = WM831X_LDO2_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO2,
		.end   = WM831X_IRQ_UV_LDO2,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo3_resources[] = {
	{
		.start = WM831X_LDO3_CONTROL,
		.end   = WM831X_LDO3_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO3,
		.end   = WM831X_IRQ_UV_LDO3,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo4_resources[] = {
	{
		.start = WM831X_LDO4_CONTROL,
		.end   = WM831X_LDO4_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO4,
		.end   = WM831X_IRQ_UV_LDO4,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo5_resources[] = {
	{
		.start = WM831X_LDO5_CONTROL,
		.end   = WM831X_LDO5_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO5,
		.end   = WM831X_IRQ_UV_LDO5,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo6_resources[] = {
	{
		.start = WM831X_LDO6_CONTROL,
		.end   = WM831X_LDO6_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO6,
		.end   = WM831X_IRQ_UV_LDO6,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo7_resources[] = {
	{
		.start = WM831X_LDO7_CONTROL,
		.end   = WM831X_LDO7_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO7,
		.end   = WM831X_IRQ_UV_LDO7,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo8_resources[] = {
	{
		.start = WM831X_LDO8_CONTROL,
		.end   = WM831X_LDO8_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO8,
		.end   = WM831X_IRQ_UV_LDO8,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo9_resources[] = {
	{
		.start = WM831X_LDO9_CONTROL,
		.end   = WM831X_LDO9_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO9,
		.end   = WM831X_IRQ_UV_LDO9,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo10_resources[] = {
	{
		.start = WM831X_LDO10_CONTROL,
		.end   = WM831X_LDO10_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
	{
		.name  = "UV",
		.start = WM831X_IRQ_UV_LDO10,
		.end   = WM831X_IRQ_UV_LDO10,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_ldo11_resources[] = {
	{
		.start = WM831X_LDO11_ON_CONTROL,
		.end   = WM831X_LDO11_SLEEP_CONTROL,
		.flags = IORESOURCE_IO,
	},
};

static struct resource wm831x_on_resources[] = {
	{
		.start = WM831X_IRQ_ON,
		.end   = WM831X_IRQ_ON,
		.flags = IORESOURCE_IRQ,
	},
};


static struct resource wm831x_power_resources[] = {
	{
		.name = "SYSLO",
		.start = WM831X_IRQ_PPM_SYSLO,
		.end   = WM831X_IRQ_PPM_SYSLO,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "PWR SRC",
		.start = WM831X_IRQ_PPM_PWR_SRC,
		.end   = WM831X_IRQ_PPM_PWR_SRC,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "USB CURR",
		.start = WM831X_IRQ_PPM_USB_CURR,
		.end   = WM831X_IRQ_PPM_USB_CURR,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "BATT HOT",
		.start = WM831X_IRQ_CHG_BATT_HOT,
		.end   = WM831X_IRQ_CHG_BATT_HOT,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "BATT COLD",
		.start = WM831X_IRQ_CHG_BATT_COLD,
		.end   = WM831X_IRQ_CHG_BATT_COLD,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "BATT FAIL",
		.start = WM831X_IRQ_CHG_BATT_FAIL,
		.end   = WM831X_IRQ_CHG_BATT_FAIL,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "OV",
		.start = WM831X_IRQ_CHG_OV,
		.end   = WM831X_IRQ_CHG_OV,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "END",
		.start = WM831X_IRQ_CHG_END,
		.end   = WM831X_IRQ_CHG_END,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "TO",
		.start = WM831X_IRQ_CHG_TO,
		.end   = WM831X_IRQ_CHG_TO,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "MODE",
		.start = WM831X_IRQ_CHG_MODE,
		.end   = WM831X_IRQ_CHG_MODE,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "START",
		.start = WM831X_IRQ_CHG_START,
		.end   = WM831X_IRQ_CHG_START,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_rtc_resources[] = {
	{
		.name = "PER",
		.start = WM831X_IRQ_RTC_PER,
		.end   = WM831X_IRQ_RTC_PER,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "ALM",
		.start = WM831X_IRQ_RTC_ALM,
		.end   = WM831X_IRQ_RTC_ALM,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_status1_resources[] = {
	{
		.start = WM831X_STATUS_LED_1,
		.end   = WM831X_STATUS_LED_1,
		.flags = IORESOURCE_IO,
	},
};

static struct resource wm831x_status2_resources[] = {
	{
		.start = WM831X_STATUS_LED_2,
		.end   = WM831X_STATUS_LED_2,
		.flags = IORESOURCE_IO,
	},
};

static struct resource wm831x_touch_resources[] = {
	{
		.name = "TCHPD",
		.start = WM831X_IRQ_TCHPD,
		.end   = WM831X_IRQ_TCHPD,
		.flags = IORESOURCE_IRQ,
	},
	{
		.name = "TCHDATA",
		.start = WM831X_IRQ_TCHDATA,
		.end   = WM831X_IRQ_TCHDATA,
		.flags = IORESOURCE_IRQ,
	},
};

static struct resource wm831x_wdt_resources[] = {
	{
		.start = WM831X_IRQ_WDOG_TO,
		.end   = WM831X_IRQ_WDOG_TO,
		.flags = IORESOURCE_IRQ,
	},
};

static struct mfd_cell wm8310_devs[] = {
	{
		.name = "wm831x-backup",
	},
	{
		.name = "wm831x-buckv",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_dcdc1_resources),
		.resources = wm831x_dcdc1_resources,
	},
	{
		.name = "wm831x-buckv",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_dcdc2_resources),
		.resources = wm831x_dcdc2_resources,
	},
	{
		.name = "wm831x-buckp",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_dcdc3_resources),
		.resources = wm831x_dcdc3_resources,
	},
	{
		.name = "wm831x-boostp",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_dcdc4_resources),
		.resources = wm831x_dcdc4_resources,
	},
	{
		.name = "wm831x-epe",
		.id = 1,
	},
	{
		.name = "wm831x-epe",
		.id = 2,
	},
	{
		.name = "wm831x-gpio",
		.num_resources = ARRAY_SIZE(wm831x_gpio_resources),
		.resources = wm831x_gpio_resources,
	},
	{
		.name = "wm831x-hwmon",
	},
	{
		.name = "wm831x-isink",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_isink1_resources),
		.resources = wm831x_isink1_resources,
	},
	{
		.name = "wm831x-isink",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_isink2_resources),
		.resources = wm831x_isink2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_ldo1_resources),
		.resources = wm831x_ldo1_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_ldo2_resources),
		.resources = wm831x_ldo2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_ldo3_resources),
		.resources = wm831x_ldo3_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_ldo4_resources),
		.resources = wm831x_ldo4_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 5,
		.num_resources = ARRAY_SIZE(wm831x_ldo5_resources),
		.resources = wm831x_ldo5_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 6,
		.num_resources = ARRAY_SIZE(wm831x_ldo6_resources),
		.resources = wm831x_ldo6_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 7,
		.num_resources = ARRAY_SIZE(wm831x_ldo7_resources),
		.resources = wm831x_ldo7_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 8,
		.num_resources = ARRAY_SIZE(wm831x_ldo8_resources),
		.resources = wm831x_ldo8_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 9,
		.num_resources = ARRAY_SIZE(wm831x_ldo9_resources),
		.resources = wm831x_ldo9_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 10,
		.num_resources = ARRAY_SIZE(wm831x_ldo10_resources),
		.resources = wm831x_ldo10_resources,
	},
	{
		.name = "wm831x-alive-ldo",
		.id = 11,
		.num_resources = ARRAY_SIZE(wm831x_ldo11_resources),
		.resources = wm831x_ldo11_resources,
	},
	{
		.name = "wm831x-on",
		.num_resources = ARRAY_SIZE(wm831x_on_resources),
		.resources = wm831x_on_resources,
	},
	{
		.name = "wm831x-power",
		.num_resources = ARRAY_SIZE(wm831x_power_resources),
		.resources = wm831x_power_resources,
	},
	{
		.name = "wm831x-rtc",
		.num_resources = ARRAY_SIZE(wm831x_rtc_resources),
		.resources = wm831x_rtc_resources,
	},
	{
		.name = "wm831x-status",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_status1_resources),
		.resources = wm831x_status1_resources,
	},
	{
		.name = "wm831x-status",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_status2_resources),
		.resources = wm831x_status2_resources,
	},
	{
		.name = "wm831x-watchdog",
		.num_resources = ARRAY_SIZE(wm831x_wdt_resources),
		.resources = wm831x_wdt_resources,
	},
};

static struct mfd_cell wm8311_devs[] = {
	{
		.name = "wm831x-backup",
	},
	{
		.name = "wm831x-buckv",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_dcdc1_resources),
		.resources = wm831x_dcdc1_resources,
	},
	{
		.name = "wm831x-buckv",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_dcdc2_resources),
		.resources = wm831x_dcdc2_resources,
	},
	{
		.name = "wm831x-buckp",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_dcdc3_resources),
		.resources = wm831x_dcdc3_resources,
	},
	{
		.name = "wm831x-boostp",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_dcdc4_resources),
		.resources = wm831x_dcdc4_resources,
	},
	{
		.name = "wm831x-epe",
		.id = 1,
	},
	{
		.name = "wm831x-epe",
		.id = 2,
	},
	{
		.name = "wm831x-gpio",
		.num_resources = ARRAY_SIZE(wm831x_gpio_resources),
		.resources = wm831x_gpio_resources,
	},
	{
		.name = "wm831x-hwmon",
	},
	{
		.name = "wm831x-isink",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_isink1_resources),
		.resources = wm831x_isink1_resources,
	},
	{
		.name = "wm831x-isink",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_isink2_resources),
		.resources = wm831x_isink2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_ldo1_resources),
		.resources = wm831x_ldo1_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_ldo2_resources),
		.resources = wm831x_ldo2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_ldo3_resources),
		.resources = wm831x_ldo3_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_ldo4_resources),
		.resources = wm831x_ldo4_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 5,
		.num_resources = ARRAY_SIZE(wm831x_ldo5_resources),
		.resources = wm831x_ldo5_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 7,
		.num_resources = ARRAY_SIZE(wm831x_ldo7_resources),
		.resources = wm831x_ldo7_resources,
	},
	{
		.name = "wm831x-alive-ldo",
		.id = 11,
		.num_resources = ARRAY_SIZE(wm831x_ldo11_resources),
		.resources = wm831x_ldo11_resources,
	},
	{
		.name = "wm831x-on",
		.num_resources = ARRAY_SIZE(wm831x_on_resources),
		.resources = wm831x_on_resources,
	},
	{
		.name = "wm831x-power",
		.num_resources = ARRAY_SIZE(wm831x_power_resources),
		.resources = wm831x_power_resources,
	},
	{
		.name = "wm831x-rtc",
		.num_resources = ARRAY_SIZE(wm831x_rtc_resources),
		.resources = wm831x_rtc_resources,
	},
	{
		.name = "wm831x-status",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_status1_resources),
		.resources = wm831x_status1_resources,
	},
	{
		.name = "wm831x-status",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_status2_resources),
		.resources = wm831x_status2_resources,
	},
	{
		.name = "wm831x-touch",
		.num_resources = ARRAY_SIZE(wm831x_touch_resources),
		.resources = wm831x_touch_resources,
	},
	{
		.name = "wm831x-watchdog",
		.num_resources = ARRAY_SIZE(wm831x_wdt_resources),
		.resources = wm831x_wdt_resources,
	},
};

static struct mfd_cell wm8312_devs[] = {
	{
		.name = "wm831x-backup",
	},
	{
		.name = "wm831x-buckv",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_dcdc1_resources),
		.resources = wm831x_dcdc1_resources,
	},
	{
		.name = "wm831x-buckv",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_dcdc2_resources),
		.resources = wm831x_dcdc2_resources,
	},
	{
		.name = "wm831x-buckp",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_dcdc3_resources),
		.resources = wm831x_dcdc3_resources,
	},
	{
		.name = "wm831x-boostp",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_dcdc4_resources),
		.resources = wm831x_dcdc4_resources,
	},
	{
		.name = "wm831x-epe",
		.id = 1,
	},
	{
		.name = "wm831x-epe",
		.id = 2,
	},
	{
		.name = "wm831x-gpio",
		.num_resources = ARRAY_SIZE(wm831x_gpio_resources),
		.resources = wm831x_gpio_resources,
	},
	{
		.name = "wm831x-hwmon",
	},
	{
		.name = "wm831x-isink",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_isink1_resources),
		.resources = wm831x_isink1_resources,
	},
	{
		.name = "wm831x-isink",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_isink2_resources),
		.resources = wm831x_isink2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_ldo1_resources),
		.resources = wm831x_ldo1_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_ldo2_resources),
		.resources = wm831x_ldo2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_ldo3_resources),
		.resources = wm831x_ldo3_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_ldo4_resources),
		.resources = wm831x_ldo4_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 5,
		.num_resources = ARRAY_SIZE(wm831x_ldo5_resources),
		.resources = wm831x_ldo5_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 6,
		.num_resources = ARRAY_SIZE(wm831x_ldo6_resources),
		.resources = wm831x_ldo6_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 7,
		.num_resources = ARRAY_SIZE(wm831x_ldo7_resources),
		.resources = wm831x_ldo7_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 8,
		.num_resources = ARRAY_SIZE(wm831x_ldo8_resources),
		.resources = wm831x_ldo8_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 9,
		.num_resources = ARRAY_SIZE(wm831x_ldo9_resources),
		.resources = wm831x_ldo9_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 10,
		.num_resources = ARRAY_SIZE(wm831x_ldo10_resources),
		.resources = wm831x_ldo10_resources,
	},
	{
		.name = "wm831x-alive-ldo",
		.id = 11,
		.num_resources = ARRAY_SIZE(wm831x_ldo11_resources),
		.resources = wm831x_ldo11_resources,
	},
	{
		.name = "wm831x-on",
		.num_resources = ARRAY_SIZE(wm831x_on_resources),
		.resources = wm831x_on_resources,
	},
	{
		.name = "wm831x-power",
		.num_resources = ARRAY_SIZE(wm831x_power_resources),
		.resources = wm831x_power_resources,
	},
	{
		.name = "wm831x-rtc",
		.num_resources = ARRAY_SIZE(wm831x_rtc_resources),
		.resources = wm831x_rtc_resources,
	},
	{
		.name = "wm831x-status",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_status1_resources),
		.resources = wm831x_status1_resources,
	},
	{
		.name = "wm831x-status",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_status2_resources),
		.resources = wm831x_status2_resources,
	},
	{
		.name = "wm831x-touch",
		.num_resources = ARRAY_SIZE(wm831x_touch_resources),
		.resources = wm831x_touch_resources,
	},
	{
		.name = "wm831x-watchdog",
		.num_resources = ARRAY_SIZE(wm831x_wdt_resources),
		.resources = wm831x_wdt_resources,
	},
};

static struct mfd_cell wm8320_devs[] = {
	{
		.name = "wm831x-backup",
	},
	{
		.name = "wm831x-buckv",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_dcdc1_resources),
		.resources = wm831x_dcdc1_resources,
	},
	{
		.name = "wm831x-buckv",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_dcdc2_resources),
		.resources = wm831x_dcdc2_resources,
	},
	{
		.name = "wm831x-buckp",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_dcdc3_resources),
		.resources = wm831x_dcdc3_resources,
	},
	{
		.name = "wm831x-buckp",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm8320_dcdc4_buck_resources),
		.resources = wm8320_dcdc4_buck_resources,
	},
	{
		.name = "wm831x-gpio",
		.num_resources = ARRAY_SIZE(wm831x_gpio_resources),
		.resources = wm831x_gpio_resources,
	},
	{
		.name = "wm831x-hwmon",
	},
	{
		.name = "wm831x-ldo",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_ldo1_resources),
		.resources = wm831x_ldo1_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_ldo2_resources),
		.resources = wm831x_ldo2_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 3,
		.num_resources = ARRAY_SIZE(wm831x_ldo3_resources),
		.resources = wm831x_ldo3_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 4,
		.num_resources = ARRAY_SIZE(wm831x_ldo4_resources),
		.resources = wm831x_ldo4_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 5,
		.num_resources = ARRAY_SIZE(wm831x_ldo5_resources),
		.resources = wm831x_ldo5_resources,
	},
	{
		.name = "wm831x-ldo",
		.id = 6,
		.num_resources = ARRAY_SIZE(wm831x_ldo6_resources),
		.resources = wm831x_ldo6_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 7,
		.num_resources = ARRAY_SIZE(wm831x_ldo7_resources),
		.resources = wm831x_ldo7_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 8,
		.num_resources = ARRAY_SIZE(wm831x_ldo8_resources),
		.resources = wm831x_ldo8_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 9,
		.num_resources = ARRAY_SIZE(wm831x_ldo9_resources),
		.resources = wm831x_ldo9_resources,
	},
	{
		.name = "wm831x-aldo",
		.id = 10,
		.num_resources = ARRAY_SIZE(wm831x_ldo10_resources),
		.resources = wm831x_ldo10_resources,
	},
	{
		.name = "wm831x-alive-ldo",
		.id = 11,
		.num_resources = ARRAY_SIZE(wm831x_ldo11_resources),
		.resources = wm831x_ldo11_resources,
	},
	{
		.name = "wm831x-on",
		.num_resources = ARRAY_SIZE(wm831x_on_resources),
		.resources = wm831x_on_resources,
	},
	{
		.name = "wm831x-rtc",
		.num_resources = ARRAY_SIZE(wm831x_rtc_resources),
		.resources = wm831x_rtc_resources,
	},
	{
		.name = "wm831x-status",
		.id = 1,
		.num_resources = ARRAY_SIZE(wm831x_status1_resources),
		.resources = wm831x_status1_resources,
	},
	{
		.name = "wm831x-status",
		.id = 2,
		.num_resources = ARRAY_SIZE(wm831x_status2_resources),
		.resources = wm831x_status2_resources,
	},
	{
		.name = "wm831x-watchdog",
		.num_resources = ARRAY_SIZE(wm831x_wdt_resources),
		.resources = wm831x_wdt_resources,
	},
};

static struct mfd_cell backlight_devs[] = {
	{
		.name = "wm831x-backlight",
	},
};

void turn_off_v29_vcc_at_standby(void)
{
	wm831x_reg_write(the_wm831x,0x4065,0x0980);
}
EXPORT_SYMBOL(turn_off_v29_vcc_at_standby);

void turn_on_v29_vcc_at_standby(void)
{
	wm831x_reg_write(the_wm831x,0x4065,0x0180);
}
EXPORT_SYMBOL(turn_on_v29_vcc_at_standby);

void wm831x_rtc_alarm_disable(void)
{
	wm831x_set_bits(the_wm831x, WM831X_RTC_CONTROL, WM831X_RTC_ALM_ENA, 0);
}
EXPORT_SYMBOL(wm831x_rtc_alarm_disable);

/*
 * Instantiate the generic non-control parts of the device.
 */
int wm831x_device_init(struct wm831x *wm831x, unsigned long id, int irq)
{
	struct wm831x_pdata *pdata = wm831x->dev->platform_data;
	int rev;
	enum wm831x_parent parent;
	int ret, i;

	the_wm831x = wm831x;

	mutex_init(&wm831x->io_lock);
	mutex_init(&wm831x->key_lock);
	mutex_init(&wm831x->auxadc_lock);
	init_completion(&wm831x->auxadc_done);
	dev_set_drvdata(wm831x->dev, wm831x);

	ret = wm831x_reg_read(wm831x, WM831X_PARENT_ID);
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to read parent ID: %d\n", ret);
		goto err;
	}
	switch (ret) {
	case 0x6204:
	case 0x6246:
		break;
	default:
		dev_err(wm831x->dev, "Device is not a WM831x: ID %x\n", ret);
		ret = -EINVAL;
		goto err;
	}

	//ret = wm831x_reg_read(wm831x, WM831X_REVISION);
	ret = 0x02FF;
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to read revision: %d\n", ret);
		goto err;
	}
	rev = (ret & WM831X_PARENT_REV_MASK) >> WM831X_PARENT_REV_SHIFT;

	//ret = wm831x_reg_read(wm831x, WM831X_RESET_ID);
	ret = 0x8321;
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to read device ID: %d\n", ret);
		goto err;
	}

	/* Some engineering samples do not have the ID set, rely on
	 * the device being registered correctly.
	 */
	if (ret == 0) {
		dev_info(wm831x->dev, "Device is an engineering sample\n");
		ret = id;
	}

	switch (ret) {
	case WM8310:
		parent = WM8310;
		wm831x->num_gpio = 16;
		wm831x->charger_irq_wake = 1;
		if (rev > 0) {
			wm831x->has_gpio_ena = 1;
			wm831x->has_cs_sts = 1;
		}

		dev_info(wm831x->dev, "WM8310 revision %c\n", 'A' + rev);
		break;

	case WM8311:
		parent = WM8311;
		wm831x->num_gpio = 16;
		wm831x->charger_irq_wake = 1;
		if (rev > 0) {
			wm831x->has_gpio_ena = 1;
			wm831x->has_cs_sts = 1;
		}

		dev_info(wm831x->dev, "WM8311 revision %c\n", 'A' + rev);
		break;

	case WM8312:
		parent = WM8312;
		wm831x->num_gpio = 16;
		wm831x->charger_irq_wake = 1;
		if (rev > 0) {
			wm831x->has_gpio_ena = 1;
			wm831x->has_cs_sts = 1;
		}

		dev_info(wm831x->dev, "WM8312 revision %c\n", 'A' + rev);
		break;

	case WM8320:
		parent = WM8320;
		wm831x->num_gpio = 12;
		dev_info(wm831x->dev, "WM8320 revision %c\n", 'A' + rev);
		break;

	case WM8321:
		parent = WM8321;
		wm831x->num_gpio = 12;
		dev_info(wm831x->dev, "WM8321 revision %c\n", 'A' + rev);
		break;

	case WM8325:
		parent = WM8325;
		wm831x->num_gpio = 12;
		dev_info(wm831x->dev, "WM8325 revision %c\n", 'A' + rev);
		break;

	case WM8326:
		parent = WM8326;
		wm831x->num_gpio = 12;
		dev_info(wm831x->dev, "WM8326 revision %c\n", 'A' + rev);
		break;

	default:
		dev_err(wm831x->dev, "Unknown WM831x device %04x\n", ret);
		ret = -EINVAL;
		goto err;
	}

	/* This will need revisiting in future but is OK for all
	 * current parts.
	 */
	if (parent != id)
		dev_warn(wm831x->dev, "Device was registered as a WM%lx\n",
			 id);

	/* Bootstrap the user key */
	ret = wm831x_reg_read(wm831x, WM831X_SECURITY_KEY);
	if (ret < 0) {
		dev_err(wm831x->dev, "Failed to read security key: %d\n", ret);
		goto err;
	}
	if (ret != 0) {
		dev_warn(wm831x->dev, "Security key had non-zero value %x\n",
			 ret);
		wm831x_reg_write(wm831x, WM831X_SECURITY_KEY, 0);
	}
	wm831x->locked = 1;

	if (pdata && pdata->pre_init) {
		ret = pdata->pre_init(wm831x);
		if (ret != 0) {
			dev_err(wm831x->dev, "pre_init() failed: %d\n", ret);
			goto err;
		}
	}

	if (pdata) {
		for (i = 0; i < ARRAY_SIZE(pdata->gpio_defaults); i++) {
			if (!pdata->gpio_defaults[i])
				continue;

			wm831x_reg_write(wm831x,
					 WM831X_GPIO1_CONTROL + i,
					 pdata->gpio_defaults[i] & 0xffff);
		}
	}
	
	ret = wm831x_irq_init(wm831x, irq);
	if (ret != 0)
		goto err;
	
	if (wm831x->irq_base) {
		ret = request_threaded_irq(wm831x->irq_base +
					   WM831X_IRQ_AUXADC_DATA,
					   NULL, wm831x_auxadc_irq, 0,
					   "auxadc", wm831x);
		if (ret < 0)
			dev_err(wm831x->dev, "AUXADC IRQ request failed: %d\n",
				ret);
		
		ret = request_threaded_irq(wm831x->irq_base + WM831X_IRQ_GPIO_9,
									NULL, wake_request_from_sub_cpu,
									IRQF_TRIGGER_RISING,
									"wake_request_from_sub_cpu", wm831x);
		
		if (ret < 0)
			dev_err(wm831x->dev, "wake_request_from_sub_cpu: could not claim irq: %d\n", ret);

	}

	/* The core device is up, instantiate the subdevices. */
	switch (parent) {
	case WM8310:
		ret = mfd_add_devices(wm831x->dev, -1,
				      wm8310_devs, ARRAY_SIZE(wm8310_devs),
				      NULL, wm831x->irq_base);
		break;

	case WM8311:
		ret = mfd_add_devices(wm831x->dev, -1,
				      wm8311_devs, ARRAY_SIZE(wm8311_devs),
				      NULL, wm831x->irq_base);
		break;

	case WM8312:
		ret = mfd_add_devices(wm831x->dev, -1,
				      wm8312_devs, ARRAY_SIZE(wm8312_devs),
				      NULL, wm831x->irq_base);
		break;

	case WM8320:
	case WM8321:
	case WM8325:
	case WM8326:
		ret = mfd_add_devices(wm831x->dev, -1,
				      wm8320_devs, ARRAY_SIZE(wm8320_devs),
				      NULL, wm831x->irq_base);
		break;

	default:
		/* If this happens the bus probe function is buggy */
		BUG();
	}

	if (ret != 0) {
		dev_err(wm831x->dev, "Failed to add children\n");
		goto err_irq;
	}

	if (pdata && pdata->backlight) {
		/* Treat errors as non-critical */
		ret = mfd_add_devices(wm831x->dev, -1, backlight_devs,
				      ARRAY_SIZE(backlight_devs), NULL,
				      wm831x->irq_base);
		if (ret < 0)
			dev_err(wm831x->dev, "Failed to add backlight: %d\n",
				ret);
	}
	
	wm831x_otp_init(wm831x);

	if (pdata && pdata->post_init) {
		ret = pdata->post_init(wm831x);
		if (ret != 0) {
			dev_err(wm831x->dev, "post_init() failed: %d\n", ret);
			goto err_irq;
		}
	}
	
wm831x_init_debugfs(wm831x);

	return 0;

err_irq:
	wm831x_irq_exit(wm831x);
err:
	mfd_remove_devices(wm831x->dev);
	kfree(wm831x);
	return ret;
}

void wm831x_device_exit(struct wm831x *wm831x)
{
	wm831x_otp_exit(wm831x);
	mfd_remove_devices(wm831x->dev);
	if (wm831x->irq_base)
		free_irq(wm831x->irq_base + WM831X_IRQ_AUXADC_DATA, wm831x);
	wm831x_irq_exit(wm831x);
	kfree(wm831x);
}

int wm831x_device_suspend(struct wm831x *wm831x)
{
	int reg, mask;

	/* If the charger IRQs are a wake source then make sure we ack
	 * them even if they're not actively being used (eg, no power
	 * driver or no IRQ line wired up) then acknowledge the
	 * interrupts otherwise suspend won't last very long.
	 */
	if (wm831x->charger_irq_wake) {
		reg = wm831x_reg_read(wm831x, WM831X_INTERRUPT_STATUS_2_MASK);

		mask = WM831X_CHG_BATT_HOT_EINT |
			WM831X_CHG_BATT_COLD_EINT |
			WM831X_CHG_BATT_FAIL_EINT |
			WM831X_CHG_OV_EINT | WM831X_CHG_END_EINT |
			WM831X_CHG_TO_EINT | WM831X_CHG_MODE_EINT |
			WM831X_CHG_START_EINT;

		/* If any of the interrupts are masked read the statuses */
		if (reg & mask)
			reg = wm831x_reg_read(wm831x,
					      WM831X_INTERRUPT_STATUS_2);

		if (reg & mask) {
			dev_info(wm831x->dev,
				 "Acknowledging masked charger IRQs: %x\n",
				 reg & mask);
			wm831x_reg_write(wm831x, WM831X_INTERRUPT_STATUS_2,
					 reg & mask);
		}
	}

	return 0;
}

MODULE_DESCRIPTION("Core support for the WM831X AudioPlus PMIC");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Brown");
