/*
 * drivers/rtc/rtc-ricoh619.c
 *
 * Real time clock driver for RICOH R5T619 power management chip.
 *
 * Copyright (C) 2012-2014 RICOH COMPANY,LTD
 *
 * Based on code
 *  Copyright (C) 2011 NVIDIA Corporation
 *
 * this program is free software; you can redistribute it and/or modify
 * it under the terms of the gnu general public license as published by
 * the free software foundation; either version 2 of the license, or
 * (at your option) any later version.
 *
 * this program is distributed in the hope that it will be useful, but without
 * any warranty; without even the implied warranty of merchantability or
 * fitness for a particular purpose.  see the gnu general public license for
 * more details.
 *
 * you should have received a copy of the gnu general public license
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/* #define debug		1 */
/* #define verbose_debug	1 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/ricoh619.h>
#include <linux/rtc/rtc-ricoh619.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/irqdomain.h>

struct ricoh61x_rtc {
	int			irq;
	struct rtc_device	*rtc;
	bool		irq_en;
};

volatile int giRicoh61x_rtc_user_enabled = 0;

static int ricoh61x_read_regs(struct device *dev, int reg, int len,
	uint8_t *val)
{
	int ret;

	ret = ricoh61x_bulk_reads(dev->parent, reg, len, val);
	if (ret < 0) {
		dev_err(dev->parent, "PMU: %s failed reading from 0x%02x\n",
			__func__, reg);
		WARN_ON(1);
	}
	return ret;
}

static int ricoh61x_write_regs(struct device *dev, int reg, int len,
	uint8_t *val)
{
	int ret;
	ret = ricoh61x_bulk_writes(dev->parent, reg, len, val);
	if (ret < 0) {
		dev_err(dev->parent, "PMU: %s failed writing\n", __func__);
		WARN_ON(1);
	}

	return ret;
}

/* 0=OK, -EINVAL= FAIL */
static int ricoh61x_rtc_valid_tm(struct device *dev, struct rtc_time *tm)
{
	if (tm->tm_year > 199 || tm->tm_year < 70
		|| tm->tm_mon > 11 || tm->tm_mon < 0
		|| tm->tm_mday < 1
		|| tm->tm_mday > rtc_month_days(tm->tm_mon, tm->tm_year + os_ref_year)
		|| tm->tm_hour >= 24 || tm->tm_hour < 0
		|| tm->tm_min < 0 || tm->tm_min >= 60
		|| tm->tm_sec < 0 || tm->tm_sec >= 60
		) {
		dev_err(dev->parent, "PMU: %s *** Returning error due to time, %d/%d/%d %d:%d:%d *****\n",
			__func__, tm->tm_mon, tm->tm_mday, tm->tm_year, tm->tm_hour, tm->tm_min, tm->tm_sec);

		return -EINVAL;
	}

	return 0;
}

static u8 dec2bcd(u8 dec)
{
	return ((dec/10)<<4)+(dec%10);
}

static u8 bcd2dec(u8 bcd)
{
	return (bcd >> 4)*10+(bcd & 0xf);
}

static void convert_bcd_to_decimal(u8 *buf, u8 len)
{
	int i = 0;
	for (i = 0; i < len; i++)
		buf[i] = bcd2dec(buf[i]);
}

static void convert_decimal_to_bcd(u8 *buf, u8 len)
{
	int i = 0;
	for (i = 0; i < len; i++)
		buf[i] = dec2bcd(buf[i]);
}

static void print_time(struct device *dev, struct rtc_time *tm)
{
	dev_info(dev, "PMU: %s *** rtc-time : %d/%d/%d %d:%d:%d *****\n",
		__func__, (tm->tm_mon), tm->tm_mday, (tm->tm_year + os_ref_year), tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static int ricoh61x_rtc_periodic_disable(struct device *dev)
{
	int err;
	uint8_t reg_data;

	/* disable function */
	err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s read rtc_ctrl1 error=0x%x\n", __func__,  err);
		return err;
	}
	reg_data &= 0xf8;
	err = ricoh61x_write_regs(dev, rtc_ctrl1, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s write rtc_ctrl1 error=0x%x\n", __func__,  err);
		return err;
	}

	/* clear alarm flag and CTFG */
	err = ricoh61x_read_regs(dev, rtc_ctrl2, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s read rtc_ctrl2 error=0x%x\n", __func__, err);
		return err;
	}
	reg_data &= ~0x85;	/* 1000-0101 */
	err = ricoh61x_write_regs(dev, rtc_ctrl2, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s write rtc_ctrl2 error=0x%x\n", __func__, err);
		return err;
	}

	return 0;
}

static int ricoh61x_rtc_clk_adjust(struct device *dev, uint8_t clk)
{ 
	return ricoh61x_write_regs(dev, rtc_adjust, 1, &clk);
}

static int ricoh61x_rtc_Pon_get_clr(struct device *dev, uint8_t *Pon_f)
{
	int err;
	uint8_t reg_data;

	err = ricoh61x_read_regs(dev, rtc_ctrl2, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "rtc_ctrl1 read err=0x%x\n", err);
		return err;
	}
	printk(KERN_INFO "%s, PON=1 -- CTRL2=0x%x\n", __func__, reg_data);

	if (reg_data & 0x10) {
		*Pon_f = 1;
		/* clear VDET PON */
		reg_data &= ~0x5b;	/* 0101-1011 */
		reg_data |= 0x20;	/* 0010-0000 */
		err = ricoh61x_write_regs(dev, rtc_ctrl2, 1, &reg_data);
		if (err < 0) {
			dev_err(dev->parent, "PMU: %s rtc_ctrl1 write err=0x%x\n", __func__, err);
		}
	} else {
		*Pon_f = 0;
	}

	return err;
}

/* 0-12hour, 1-24hour */
static int ricoh61x_rtc_hour_mode_get(struct device *dev, int *mode)
{
	int err;

	err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, mode);
	if (err < 0)
		dev_err(dev->parent, "PMU: %s read rtc ctrl1 error\n", __func__);

	if (*mode & 0x20)
		*mode = 1;
	else
		*mode = 0;

	return err;
}

/* 0-12hour, 1-24hour */
static int ricoh61x_rtc_hour_mode_set(struct device *dev, int mode)
{
	uint8_t reg_data;
	int err;

	err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s read rtc_ctrl1 error\n", __func__);
		return err;
	}
	if (mode == 0)
		reg_data &= 0xDF;
	else
		reg_data |= 0x20;
	err = ricoh61x_write_regs(dev, rtc_ctrl1, 1, &reg_data);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s write rtc_ctrl1 error\n", __func__);
	}

	return err;
}


static int ricoh61x_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	u8 buff[7];
	int err;
	int cent_flag;

//	printk(KERN_INFO "PMU: %s\n", __func__);
	err = ricoh61x_read_regs(dev, rtc_seconds_reg, sizeof(buff), buff);

	if (err < 0) {
		dev_err(dev->parent, "PMU: %s *** failed to read time *****\n", __func__);
		return err;
	}

	if (buff[5] & 0x80)
		cent_flag = 1;
	else
		cent_flag = 0;

	buff[5] = buff[5]&0x1f;		/* bit5 19_20 */
	convert_bcd_to_decimal(buff, sizeof(buff));

	tm->tm_sec  = buff[0];
	tm->tm_min  = buff[1];
	tm->tm_hour = buff[2];		/* bit5 PA_H20 */
	tm->tm_wday = buff[3];
	tm->tm_mday = buff[4];
	tm->tm_mon  = buff[5];		/* for print */
	tm->tm_year = buff[6] + 100 * cent_flag;
//	print_time(dev, tm);		/* for print */
	tm->tm_mon  = buff[5] - 1;	/* back to system 0-11 */

	return 0;
}

static int ricoh61x_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	u8 buff[7];
	int err;
	int cent_flag;

	printk(KERN_INFO "PMU: %s\n", __func__);

	if (ricoh61x_rtc_valid_tm(dev, tm) != 0) {
		return -EINVAL;
	}

	if (tm->tm_year >= 100)
		cent_flag = 1;
	else
		cent_flag = 0;

	tm->tm_mon = tm->tm_mon + 1;
	buff[0] = tm->tm_sec;
	buff[1] = tm->tm_min;
	buff[2] = tm->tm_hour;
	buff[3] = tm->tm_wday;
	buff[4] = tm->tm_mday;
	buff[5] = tm->tm_mon;		/* system set 0-11 */
	buff[6] = tm->tm_year - 100 * cent_flag;
	print_time(dev, tm);		/* RTC_TEST */

	convert_decimal_to_bcd(buff, sizeof(buff));

	if (1 == cent_flag)
		buff[5] |= 0x80;

	err = ricoh61x_write_regs(dev, rtc_seconds_reg, sizeof(buff), buff);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s failed to program new time\n", __func__);
		return err;
	}

	return 0;
}

static int ricoh61x_rtc_alarm_is_enabled(struct device *dev,  uint8_t *enabled)
{
	struct ricoh61x_rtc *rtc = dev_get_drvdata(dev);
	int err;
	uint8_t reg_data;

	err = 0;
	err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, &reg_data);
	if (err) {
		dev_err(dev->parent, "read rtc_ctrl1 error 0x%lx\n", err);
		*enabled = 0;
	} else {
		if (reg_data & 0x40)
			*enabled = 1;
		else
			*enabled = 0;
	}
	return err;
}

/* 0-disable, 1-enable */
static int ricoh61x_rtc_alarm_enable(struct device *dev, unsigned int enabled)
{
	struct ricoh61x_rtc *rtc = dev_get_drvdata(dev);
	int err;
	uint8_t reg_data;

	printk(KERN_DEBUG "PMU: %s :%d\n", __func__, enabled);

	err = 0;
	if (enabled) {
		rtc->irq_en = 1;
		err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, &reg_data);
		if (err < 0) {
			dev_err(dev->parent, "PMU: %s read rtc_ctrl1 error 0x%lx\n", __func__, err);
			goto ERR;
		}
		reg_data |= 0x40;	/* set DALE */
		err = ricoh61x_write_regs(dev, rtc_ctrl1, 1, &reg_data);
		if (err < 0)
			dev_err(dev->parent, "PMU: %s write rtc_ctrl1 error 0x%lx\n", __func__, err);
	} else {
		rtc->irq_en = 0;
		err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, &reg_data);
		if (err < 0) {
			dev_err(dev->parent, "PMU: %s read rtc_ctrl1 error 0x%lx\n", __func__, err);
			goto ERR;
		}
		reg_data &= 0xbf;/* clear DALE */
		err = ricoh61x_write_regs(dev, rtc_ctrl1, 1, &reg_data);
		if (err < 0)
			dev_err(dev->parent, "PMU: %s write rtc_ctrl1 error 0x%lx\n", __func__, err);
	}
	giRicoh61x_rtc_user_enabled = enabled;

ERR:
	return err;
}

static int ricoh61x_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	u8 buff[6];
	u8 buff_cent;
	int err;
	int cent_flag;
	unsigned char enabled_flag;

	printk(KERN_INFO "PMU: %s\n", __func__);

	err = 0;

	alrm->time.tm_sec  = 0;
	alrm->time.tm_min  = 0;
	alrm->time.tm_hour = 0;
	alrm->time.tm_mday = 0;
	alrm->time.tm_mon  = 0;
	alrm->time.tm_year = 0;
	alrm->enabled = 0;

	err = ricoh61x_read_regs(dev, rtc_month_reg, 1, &buff_cent);
	if (err < 0) {
		dev_err(dev->parent, "PMU: %s *** failed to read time *****\n", __func__);
		return err;
	}
	if (buff_cent & 0x80)
		cent_flag = 1;
	else
		cent_flag = 0;

	err = ricoh61x_read_regs(dev, rtc_alarm_y_sec, sizeof(buff), buff);
	if (err) {
		dev_err(dev->parent, "RTC: %s *** read rtc_alarm timer error 0x%lx\n", __func__, err);
		return err;
	}

	err = ricoh61x_read_regs(dev, rtc_ctrl1, 1, &enabled_flag);
	if (err) {
		dev_err(dev->parent, "RTC: %s *** read rtc_enable flag error 0x%lx\n", __func__, err);
		return err;
	}
	if (enabled_flag & 0x40)
		enabled_flag = 1;
	else
		enabled_flag = 0;

	buff[3] &= ~0x80;	/* clear DAL_EXT */

	buff[3] = buff[3]&0x3f;
	convert_bcd_to_decimal(buff, sizeof(buff));

	alrm->time.tm_sec  = buff[0];
	alrm->time.tm_min  = buff[1];
	alrm->time.tm_hour = buff[2];
	alrm->time.tm_mday = buff[3];
	alrm->time.tm_mon = buff[4];	/* for print */
	alrm->time.tm_year = buff[5] + 100 * cent_flag;
	dev_info(dev, "PMU: read alarm: %d/%d/%d %d:%d:%d *****\n",
		(alrm->time.tm_mon), alrm->time.tm_mday, (alrm->time.tm_year + os_ref_year), alrm->time.tm_hour, alrm->time.tm_min, alrm->time.tm_sec);
	alrm->time.tm_mon  = buff[4] - 1;
	alrm->enabled = enabled_flag;

	return 0;
}

static int ricoh61x_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct ricoh61x_rtc *rtc = dev_get_drvdata(dev);
	u8 buff[6];
	int err;
	int cent_flag;

	printk(KERN_INFO "PMU: %s\n", __func__);
	err = 0;
	ricoh61x_rtc_alarm_enable(dev, 0);
	if (rtc->irq == -1) {
		err = -EIO;
		goto ERR;
	}

	if (alrm->enabled == 0)
		return 0;

	if (alrm->time.tm_year >= 100)
		cent_flag = 1;
	else
		cent_flag = 0;

	alrm->time.tm_mon += 1;
	print_time(dev->parent, &alrm->time);
	buff[0] = alrm->time.tm_sec;
	buff[1] = alrm->time.tm_min;
	buff[2] = alrm->time.tm_hour;
	buff[3] = alrm->time.tm_mday;
	buff[4] = alrm->time.tm_mon;
	buff[5] = alrm->time.tm_year - 100 * cent_flag;
	convert_decimal_to_bcd(buff, sizeof(buff));
	buff[3] |= 0x80;	/* set DAL_EXT */
	err = ricoh61x_write_regs(dev, rtc_alarm_y_sec, sizeof(buff), buff);
	if (err) {
		dev_err(dev->parent, "PMU: %s unable to set alarm\n", __func__);
		err = -EBUSY;
		goto ERR;
	}

	ricoh61x_rtc_alarm_enable(dev, alrm->enabled);

ERR:
	return err;
}

#define RTC_WAKEUP_FLAG _IOR('p', 0x13, unsigned long)
static int ricoh61x_rtc_ioctl(struct device *dev, unsigned int cmd, unsigned long arg)
{
	if (cmd == RTC_WAKEUP_FLAG) {
		extern int g_wakeup_by_alarm;
		put_user(g_wakeup_by_alarm, (unsigned long __user *) arg);
		g_wakeup_by_alarm = 0;
		return 0;
	}
	return -ENOIOCTLCMD;
}

static const struct rtc_class_ops ricoh61x_rtc_ops = {
	.read_time	= ricoh61x_rtc_read_time,
	.set_time	= ricoh61x_rtc_set_time,
	.set_alarm	= ricoh61x_rtc_set_alarm,
	.read_alarm	= ricoh61x_rtc_read_alarm,
	.alarm_irq_enable = ricoh61x_rtc_alarm_enable,
	.ioctl		= ricoh61x_rtc_ioctl,
};

static int ricoh61x_rtc_alarm_flag_clr(struct device *dev)
{
	int err;
	uint8_t reg_data;

	/* clear alarm-D status bits.*/
	err = ricoh61x_read_regs(dev, rtc_ctrl2, 1, &reg_data);
	if (err)
		dev_err(dev->parent, "unable to read rtc_ctrl2 reg\n");

	/* to clear alarm-D flag, and set adjustment parameter */
	reg_data &= ~0x81;
	err = ricoh61x_write_regs(dev, rtc_ctrl2, 1, &reg_data);
	if (err)
		dev_err(dev->parent, "unable to program rtc_status reg\n");
	return err;
}
static irqreturn_t ricoh61x_rtc_irq(int irq, void *data)
{
	struct device *dev = data;
	struct ricoh61x_rtc *rtc = dev_get_drvdata(dev);
	extern int g_wakeup_by_alarm;

    g_wakeup_by_alarm = 1;
//	printk(KERN_INFO "PMU: %s\n", __func__);

	ricoh61x_rtc_alarm_flag_clr(dev);

	rtc_update_irq(rtc->rtc, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}


static int rtc_adjust_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	uint8_t reg_data;
	reg_data = simple_strtoul(buf, NULL, 10);
	
	printk ("ricoh61x set rtc_adjust %d\n", reg_data);
   	ricoh61x_write_regs(dev, rtc_adjust, 1, &reg_data);
	return count;
}
static int rtc_adjust_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	uint8_t reg_data;
	
	ricoh61x_read_regs(dev, rtc_adjust, 1, &reg_data);
	printk ("ricoh61x_rtc_adjust %d\n", reg_data);
	sprintf (buf, "%d", reg_data); 
	return strlen(buf);
}
static DEVICE_ATTR(ricoh61x_rtc_adjust, S_IRUGO, rtc_adjust_show, rtc_adjust_store);

#ifdef CONFIG_OF
static struct ricoh619_rtc_platform_data *ricoh619_rtc_dt_init(struct platform_device *pdev)
{
	struct device_node *nproot = pdev->dev.parent->of_node;
	struct device_node *np;
	struct ricoh619_rtc_platform_data *pdata;

	if (!nproot)
		return pdev->dev.platform_data;

	np = of_find_node_by_name(nproot, "rtc");
	if (!np) {
		dev_err(&pdev->dev, "failed to find rtc node\n");
		return NULL;
	}

	pdata = devm_kzalloc(&pdev->dev,
			sizeof(struct ricoh619_rtc_platform_data),
			GFP_KERNEL);

	of_property_read_u32(np, "ricoh,rtc-tm-year", &pdata->time.tm_year);
	of_property_read_u32(np, "ricoh,rtc-tm-month", &pdata->time.tm_mon);
	of_property_read_u32(np, "ricoh,rtc-tm-mday", &pdata->time.tm_mday);
	of_property_read_u32(np, "ricoh,rtc-tm-hour", &pdata->time.tm_hour);
	of_property_read_u32(np, "ricoh,rtc-tm-min", &pdata->time.tm_min);
	of_property_read_u32(np, "ricoh,rtc-tm-sec", &pdata->time.tm_sec);
	of_node_put(np);

	return pdata;
}
#else
static struct ricoh619_rtc_platform_data *
ricoh619_rtc_dt_init(struct platform_device *pdev)
{
	return pdev->dev.platform_data;
}
#endif

static int ricoh61x_rtc_probe(struct platform_device *pdev)
{
	struct ricoh619_rtc_platform_data *pdata;
	struct ricoh61x *ricoh619 = dev_get_drvdata(pdev->dev.parent);
	struct ricoh61x_rtc *rtc;
	struct rtc_time tm;
	uint8_t Pon_flag, Alarm_flag;
	int err;
	uint8_t buff[6];

	printk(KERN_INFO "PMU RTC: %s, ricoh61x driver run at 24H-mode\n", __func__);
	printk(KERN_INFO "PMU RTC: we never using periodic function and interrupt\n");

	pdata = ricoh619_rtc_dt_init(pdev);
	if (!pdata) {
		dev_err(&pdev->dev, "platform data isn't assigned to "
			"rtc\n");
		return -EINVAL;
	}

	rtc = devm_kzalloc(ricoh619->dev,sizeof(*rtc), GFP_KERNEL);
	if (IS_ERR(rtc)) {
		err = PTR_ERR(rtc);
		dev_err(&pdev->dev, "no enough memory for ricoh61x_rtc using\n");
		return -ENOMEM;
	}

	dev_set_drvdata(&pdev->dev, rtc);
	if (IS_ERR(rtc->rtc)) {
		err = PTR_ERR(rtc->rtc);
		goto fail;
	}

	rtc->irq  = irq_create_mapping(ricoh619->irq_domain, RICOH61x_IRQ_DALE);
	if(rtc->irq  < 0)
	{
		dev_err(&pdev->dev, "no irq specified, wakeup is disabled\n");
		rtc->irq = -1;
		rtc->irq_en = 0;
	} else 
		rtc->irq_en = 1;

	/* get interrupt flag */
	err = ricoh61x_rtc_alarm_is_enabled(&pdev->dev, &Alarm_flag);
	if (err) {
		dev_err(&pdev->dev, "5T61x RTC: Disable alarm interrupt error\n");
		goto fail;
	}

	/* get PON flag */
	err = ricoh61x_rtc_Pon_get_clr(&pdev->dev, &Pon_flag);
	if (err) {
		dev_err(&pdev->dev, "5T61x RTC: get PON flag error\n");
		goto fail;
	}

	/* disable rtc periodic function */
	err = ricoh61x_rtc_periodic_disable(&pdev->dev);
	if (err) {
		dev_err(&pdev->dev, "5T61x RTC: disable rtc periodic int error\n");
		goto fail;
	}

	/* clearing RTC Adjust register */
	err = ricoh61x_rtc_clk_adjust(&pdev->dev, 0);
	if (err) {
		dev_err(&pdev->dev, "unable to program rtc_adjust reg\n");
		err = -EBUSY;
		goto fail;
	}

	/* disable interrupt */
	err = ricoh61x_rtc_alarm_enable(&pdev->dev, 0);
	if (err) {
		dev_err(&pdev->dev, "5T61x RTC: Disable alarm interrupt error\n");
		goto fail;
	}

	/* PON=1 */
	if (Pon_flag) {
		Alarm_flag = 0;
		/* clear int flag */
		err = ricoh61x_rtc_alarm_flag_clr(&pdev->dev);
		if (err) {
			dev_err(&pdev->dev, "5T61x RTC: Pon=1 clear alarm flag error\n");
			goto fail;
		}

		/* using 24h-mode */
		err = ricoh61x_rtc_hour_mode_set(&pdev->dev, 1);
		if (err) {
			dev_err(&pdev->dev, "5T61x RTC: Pon=1 set 24h-mode error\n");
			goto fail;
		}

		/* setting the default year */
		printk(KERN_INFO "PMU: %s Set default time\n", __func__);

		pdata->time.tm_sec = 0;
		pdata->time.tm_min = 0;
		pdata->time.tm_hour = 0;
		pdata->time.tm_wday = 6;
		pdata->time.tm_mday = 1;
		pdata->time.tm_mon = 1;
		pdata->time.tm_year = 2012;
		pdata->time.tm_year -= os_ref_year;
		if (ricoh61x_rtc_valid_tm(&pdev->dev, &(pdata->time)) == 0) {
			tm.tm_sec  = pdata->time.tm_sec;
			tm.tm_min  = pdata->time.tm_min;
			tm.tm_hour = pdata->time.tm_hour;
			tm.tm_wday = pdata->time.tm_wday;
			tm.tm_mday = pdata->time.tm_mday;
			tm.tm_mon  = pdata->time.tm_mon-1;
			tm.tm_year = pdata->time.tm_year;
		} else {
			/* using the ricoh default time instead of board default time */
			dev_err(&pdev->dev, "board rtc default is erro\n");
			tm.tm_sec  = 0;
			tm.tm_min  = 0;
			tm.tm_hour = 0;
			tm.tm_wday = 4;
			tm.tm_mday = 1;
			tm.tm_mon  = 0;
			tm.tm_year = 70;
		}

		/* set default alarm time */
		if (tm.tm_year >= 100)
			buff[5] = tm.tm_year-100-1;
		else
			buff[5] = tm.tm_year-1;
		buff[0] = tm.tm_sec;
		buff[1] = tm.tm_min;
		buff[2] = tm.tm_hour;
		buff[3] = tm.tm_mday;
		buff[4] = tm.tm_mon + 1;

		err = ricoh61x_rtc_set_time(&pdev->dev, &tm);
		if (err) {
			dev_err(&pdev->dev, "5t61x RTC:\n failed to set time\n");
			goto fail;
		}

		convert_decimal_to_bcd(buff, sizeof(buff));
		buff[3] |= 0x80;	/* set DAL_EXT */

		err = ricoh61x_write_regs(&pdev->dev, rtc_alarm_y_sec, sizeof(buff), buff);
		if (err)
			printk(KERN_INFO "\n unable to set alarm\n");

	}

	device_init_wakeup(&pdev->dev, 1);

	printk(KERN_INFO "PMU: %s register rtc device\n", __func__);
	rtc->rtc = rtc_device_register(pdev->name, &pdev->dev,
				       &ricoh61x_rtc_ops, THIS_MODULE);

	/* set interrupt and enable it */
	if (rtc->irq != -1) {
		err = devm_request_threaded_irq(&pdev->dev,rtc->irq, NULL, ricoh61x_rtc_irq,
					IRQF_ONESHOT, "rtc_ricoh61x", &pdev->dev);
		if (err<0) {
			dev_err(&pdev->dev, "request IRQ:%d fail\n", rtc->irq);
			rtc->irq = -1;
			err = ricoh61x_rtc_alarm_enable(&pdev->dev, 0);
			if (err) {
				dev_err(&pdev->dev, "5T61x RTC: enable rtc alarm error\n");
				goto fail;
			}
		} else {
			/* enable wake */
			enable_irq_wake(rtc->irq);
			/* enable alarm_d */
			err = ricoh61x_rtc_alarm_enable(&pdev->dev, Alarm_flag);
			if (err) {
				dev_err(&pdev->dev, "failed rtc setup\n");
				err = -EBUSY;
				goto fail;
			}
		}
	} else {
		/* system don't want to using alarm interrupt, so close it */
		err = ricoh61x_rtc_alarm_enable(&pdev->dev, 0);
		if (err) {
			dev_err(&pdev->dev, "5T61x RTC: Disable rtc alarm error\n");
			goto fail;
		}
		dev_err(&pdev->dev, "ricoh61x interrupt is disabled\n");
	}

	printk(KERN_INFO "RICOH61x RTC Register Success\n");

	device_create_file(&pdev->dev, &dev_attr_ricoh61x_rtc_adjust);

	return 0;

fail:
	if (!IS_ERR_OR_NULL(rtc->rtc))
		rtc_device_unregister(rtc->rtc);
	kfree(rtc);
	return err;
}

static int ricoh61x_rtc_remove(struct platform_device *pdev)
{
	struct ricoh61x_rtc *rtc = dev_get_drvdata(&pdev->dev);

	if (rtc->irq != -1)
		free_irq(rtc->irq, rtc);
	rtc_device_unregister(rtc->rtc);
	kfree(rtc);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id ricoh61x_rtc_dt_match[] = {
	{ .compatible = "ricoh,ricoh619-rtc", },
	{},
};
MODULE_DEVICE_TABLE(of, ricoh61x_rtc_dt_match);
#endif

static struct platform_driver ricoh61x_rtc_driver = {
	.driver	= {
		.name	= "rtc_ricoh619",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(ricoh61x_rtc_dt_match),
	},
	.probe	= ricoh61x_rtc_probe,
	.remove	= ricoh61x_rtc_remove,
};

static int __init ricoh61x_rtc_init(void)
{
	return platform_driver_register(&ricoh61x_rtc_driver);
}
subsys_initcall_sync(ricoh61x_rtc_init);

static void __exit ricoh61x_rtc_exit(void)
{
	platform_driver_unregister(&ricoh61x_rtc_driver);
}
module_exit(ricoh61x_rtc_exit);

MODULE_DESCRIPTION("RICOH RICOH619 RTC driver");
MODULE_ALIAS("platform:rtc_ricoh619");
MODULE_LICENSE("GPL");

