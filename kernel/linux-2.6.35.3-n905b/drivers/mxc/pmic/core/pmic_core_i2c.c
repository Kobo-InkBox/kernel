/*
 * Copyright 2008-2010 Freescale Semiconductor, Inc. All Rights Reserved.
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
 * @file pmic_core_i2c.c
 * @brief This is the main file for the PMIC Core/Protocol driver. i2c
 * should be providing the interface between the PMIC and the MCU.
 *
 * @ingroup PMIC_CORE
 */

/*
 * Includes
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/mfd/mc13892/core.h>
#include <linux/pmic_external.h>
#include <linux/pmic_status.h>
#include <linux/uaccess.h>
#include <mach/hardware.h>

#include "pmic.h"

#define MC13892_GENERATION_ID_LSH	6
#define MC13892_IC_ID_LSH		13

#define MC13892_GENERATION_ID_WID	3
#define MC13892_IC_ID_WID		6

#define MC13892_GEN_ID_VALUE	0x7
#define MC13892_IC_ID_VALUE		1

/*
 * Global variables
 */
struct i2c_client *mc13892_client;

extern pmic_version_t mxc_pmic_version;
extern irqreturn_t pmic_irq_handler(int irq, void *dev_id);
/*
 * Platform device structure for PMIC client drivers
 */
static struct platform_device adc_ldm = {
	.name = "pmic_adc",
	.id = 1,
};
static struct platform_device battery_ldm = {
	.name = "pmic_battery",
	.id = 1,
};
static struct platform_device power_ldm = {
	.name = "pmic_power",
	.id = 1,
};
static struct platform_device rtc_ldm = {
	.name = "pmic_rtc",
	.id = 1,
};
static struct platform_device light_ldm = {
	.name = "pmic_light",
	.id = 1,
};
static struct platform_device rleds_ldm = {
	.name = "pmic_leds",
	.id = 'r',
};
static struct platform_device gleds_ldm = {
	.name = "pmic_leds",
	.id = 'g',
};
static struct platform_device bleds_ldm = {
	.name = "pmic_leds",
	.id = 'b',
};

static void pmic_pdev_register(struct device *dev)
{
	platform_device_register(&adc_ldm);

	if (!cpu_is_mx53())
		platform_device_register(&battery_ldm);

	platform_device_register(&rtc_ldm);
	platform_device_register(&power_ldm);
	platform_device_register(&light_ldm);
	platform_device_register(&rleds_ldm);
	platform_device_register(&gleds_ldm);
	platform_device_register(&bleds_ldm);
}

/*!
 * This function unregisters platform device structures for
 * PMIC client drivers.
 */
static void pmic_pdev_unregister(void)
{
	platform_device_unregister(&adc_ldm);
	platform_device_unregister(&battery_ldm);
	platform_device_unregister(&rtc_ldm);
	platform_device_unregister(&power_ldm);
	platform_device_unregister(&light_ldm);
}

static int __devinit is_chip_onboard(struct i2c_client *client)
{
	unsigned int ret = 0;

	/*bind the right device to the driver */
	if (pmic_i2c_24bit_read(client, REG_IDENTIFICATION, &ret) == -1)
		return -1;

	if (MC13892_GEN_ID_VALUE != BITFEXT(ret, MC13892_GENERATION_ID)) {
		/*compare the address value */
		dev_err(&client->dev,
			"read generation ID 0x%x is not equal to 0x%x!\n",
			BITFEXT(ret, MC13892_GENERATION_ID),
			MC13892_GEN_ID_VALUE);
		return -1;
	}

	return 0;
}

static ssize_t mc13892_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int i, value;
	int offset = (REG_TEST4 + 1) / 4;

	for (i = 0; i < offset; i++) {
		pmic_read(i, &value);
		pr_info("reg%02d: %06x\t\t", i, value);
		pmic_read(i + offset, &value);
		pr_info("reg%02d: %06x\t\t", i + offset, value);
		pmic_read(i + offset * 2, &value);
		pr_info("reg%02d: %06x\t\t", i + offset * 2, value);
		pmic_read(i + offset * 3, &value);
		pr_info("reg%02d: %06x\n", i + offset * 3, value);
	}

	return 0;
}

static ssize_t mc13892_store(struct device *dev,
			     struct device_attribute *attr, const char *buf,
			     size_t count)
{
	int reg, value, ret;
	char *p;

	reg = simple_strtoul(buf, NULL, 10);

	p = NULL;
	p = memchr(buf, ' ', count);

	if (p == NULL) {
		pmic_read(reg, &value);
		pr_debug("reg%02d: %06x\n", reg, value);
		return count;
	}

	p += 1;

	value = simple_strtoul(p, NULL, 16);

	ret = pmic_write(reg, value);
	if (ret == 0)
		pr_debug("write reg%02d: %06x\n", reg, value);
	else
		pr_debug("register update failed\n");

	return count;
}

static struct device_attribute mc13892_dev_attr = {
	.attr = {
		 .name = "mc13892_ctl",
		 .mode = S_IRUSR | S_IWUSR,
		 },
	.show = mc13892_show,
	.store = mc13892_store,
};

static int __devinit pmic_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	int ret = 0;
	int pmic_irq;
	struct mc13892 *mc13892;
	struct mc13892_platform_data *plat_data = client->dev.platform_data;

	ret = is_chip_onboard(client);
	if (ret == -1)
		return -ENODEV;

	mc13892 = kzalloc(sizeof(struct mc13892), GFP_KERNEL);
	if (mc13892 == NULL)
		return -ENOMEM;

	i2c_set_clientdata(client, mc13892);
	mc13892->dev = &client->dev;
	mc13892->i2c_client = client;

	/* so far, we got matched chip on board */

	mc13892_client = client;

	/* Initialize the PMIC event handling */
	pmic_event_list_init();

	/* Initialize GPIO for PMIC Interrupt */
	gpio_pmic_active();

	/* Get the PMIC Version */
	pmic_get_revision(&mxc_pmic_version);
	if (mxc_pmic_version.revision < 0) {
		dev_err((struct device *)client,
			"PMIC not detected!!! Access Failed\n");
		return -ENODEV;
	} else {
		dev_dbg((struct device *)client,
			"Detected pmic core IC version number is %d\n",
			mxc_pmic_version.revision);
	}

	/* Initialize the PMIC parameters */
	ret = pmic_init_registers();
	if (ret != PMIC_SUCCESS)
		return PMIC_ERROR;

	pmic_irq = (int)(client->irq);
	if (pmic_irq == 0)
		return PMIC_ERROR;

	ret = pmic_start_event_thread(pmic_irq);
	if (ret) {
		pr_err("mc13892 pmic driver init: \
			fail to start event thread\n");
		return PMIC_ERROR;
	}

	/* Set and install PMIC IRQ handler */

	set_irq_type(pmic_irq, IRQF_TRIGGER_HIGH);

	ret =
	    request_irq(pmic_irq, pmic_irq_handler, 0, "PMIC_IRQ",
			0);

	if (ret) {
		dev_err(&client->dev, "request irq %d error!\n", pmic_irq);
		return ret;
	}
	enable_irq_wake(pmic_irq);

	if (plat_data && plat_data->init) {
		ret = plat_data->init(mc13892);
		if (ret != 0)
			return PMIC_ERROR;
	}

	ret = device_create_file(&client->dev, &mc13892_dev_attr);
	if (ret)
		dev_err(&client->dev, "create device file failed!\n");

	pmic_pdev_register(&client->dev);

	dev_info(&client->dev, "Loaded\n");

	return PMIC_SUCCESS;
}

static int pmic_remove(struct i2c_client *client)
{
	int pmic_irq = (int)(client->irq);

	pmic_stop_event_thread();
	free_irq(pmic_irq, 0);
	pmic_pdev_unregister();
	return 0;
}

static int pmic_suspend(struct i2c_client *client, pm_message_t state)
{
	return 0;
}

static int pmic_resume(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id mc13892_id[] = {
	{"mc13892", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, mc13892_id);

static struct i2c_driver pmic_driver = {
	.driver = {
		   .name = "mc13892",
		   .bus = NULL,
		   },
	.probe = pmic_probe,
	.remove = pmic_remove,
	.suspend = pmic_suspend,
	.resume = pmic_resume,
	.id_table = mc13892_id,
};

#if 1	// Joseph 091210
struct i2c_client *msp430_client;	// Joseph 091221
extern void ntx_msp430_i2c_force_release (void);
unsigned int msp430_read(unsigned int reg)
{
	struct i2c_client *client = msp430_client;
	int i2c_ret;
	int retry_count = 5;
	u16 value = -1;
	u8 buf0[2], buf1[2]={0,0};
	u16 addr = client->addr;
	u16 flags = client->flags;
	struct i2c_msg msg[2] = {
		{addr, flags, 1, buf0},
		{addr, flags | I2C_M_RD, 2, buf1},
	};

	while (retry_count--) {
		buf0[0] = reg & 0xff;
		i2c_ret = i2c_transfer(client->adapter, msg, 2);
		if (i2c_ret >= 0) {
			value = buf1[0] << 8 | buf1[1];
			break;
		}
		pr_err("%s: read reg error : Reg 0x%02x\n", __func__, reg);
		ntx_msp430_i2c_force_release ();
	}
//	printk("[%s-%d] reg:0x%02x, value:0x%04x\n",__FUNCTION__,__LINE__, reg, value);
	return value;
}

int msp430_write(unsigned int reg, unsigned int value)
{
	struct i2c_client *client = msp430_client;
	u16 addr = client->addr;
	u16 flags = client->flags;
	u8 buf[4];
	int i2c_ret;
	struct i2c_msg msg = { addr, flags, 3, buf };

//	printk ("[%s-%d] 0x%02X 0x%04X\n",__FUNCTION__,__LINE__,reg,value);
	pr_debug("w r:%02x,v:%04x\n", reg, value);
	buf[0] = reg & 0xff;
	buf[1] = (value & 0xff00) >> 8;
	buf[2] = value & 0xff;

	i2c_ret = i2c_transfer(client->adapter, &msg, 1);
	if (i2c_ret < 0) {
		pr_err("%s: write reg error : Reg 0x%02x = 0x%04x\n",
		       __func__, reg, value);
		ntx_msp430_i2c_force_release ();
		return -EIO;
	}

	return i2c_ret;
}

static int __devinit msp430_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	/*
	int i2c_ret;
	u16 value;
	u8 buf0[2], buf1[2];
	u16 addr = client->addr;
	u16 flags = client->flags;
	struct i2c_msg msg[2] = {
		{addr, flags, 1, buf0},
		{addr, flags | I2C_M_RD, 2, buf1},
	};

	
	buf0[0] = 0;
	i2c_ret = i2c_transfer(client->adapter, msg, 2);
	if (i2c_ret < 0) {
		pr_err("%s: read reg error : Reg 0\n", __func__);
		return 0;
	}
	*/
	msp430_client = client;

	printk("[%s-%d] MSP430 firmware version %04X\n",__FUNCTION__,__LINE__, msp430_read(0));
	msp430_write(0x30,0xFF00);	// Jospeh 100108 // start ADC

	return PMIC_SUCCESS;
}

static const struct i2c_device_id msp430_id[] = {
	{"msp430", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, msp430_id);

static struct i2c_driver msp430_driver = {
	.driver = {
		   .name = "msp430",
		   .bus = NULL,
		   },
	.probe = msp430_probe,
	.id_table = msp430_id,
};
#endif

static int __init pmic_init(void)
{
#if 1
	return i2c_add_driver(&msp430_driver);		// Joseph 091210
#else
	return i2c_add_driver(&pmic_driver);
#endif
}

static void __exit pmic_exit(void)
{
#if 1
	i2c_del_driver(&msp430_driver);
#else
	i2c_del_driver(&pmic_driver);
#endif
}

/*
 * Module entry points
 */
subsys_initcall_sync(pmic_init);
module_exit(pmic_exit);

MODULE_DESCRIPTION("Core/Protocol driver for PMIC");
MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_LICENSE("GPL");
