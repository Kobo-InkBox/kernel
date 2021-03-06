/*
 * Copyright 2010-2011 Amazon.com, Inc. All rights reserved.
 * Manish Lachwani (lachwani@lab126.com)
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/err.h>
#include <linux/pmic_external.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/mc13892/core.h>
#include <linux/delay.h>
#include <mach/irqs.h>
#include <mach/boardid.h>
#include <mach/clock.h>
#include <linux/clk.h>
#include "iomux.h"
#include "mx50_pins.h"

/*
 * Convenience conversion.
 * Here atm, maybe there is somewhere better for this.
 */
#define mV_to_uV(mV) (mV * 1000)
#define uV_to_mV(uV) (uV / 1000)
#define V_to_uV(V) (mV_to_uV(V * 1000))
#define uV_to_V(uV) (uV_to_mV(uV) / 1000)

/* Coin cell charger enable */
#define COINCHEN_LSH	23
#define COINCHEN_WID	1
/* Coin cell charger voltage setting */
#define VCOIN_LSH	20
#define VCOIN_WID	3

/* Coin Charger voltage */
#define VCOIN_2_5V	0x0
#define VCOIN_2_7V	0x1
#define VCOIN_2_8V	0x2
#define VCOIN_2_9V	0x3
#define VCOIN_3_0V	0x4
#define VCOIN_3_1V	0x5
#define VCOIN_3_2V	0x6
#define VCOIN_3_3V	0x7

/* Keeps VSRTC and CLK32KMCU on for all states */
#define DRM_LSH 4
#define DRM_WID 1

/* regulator standby mask */
#define GEN1_STBY_MASK		(1 << 1)
#define IOHI_STBY_MASK		(1 << 4)
#define DIG_STBY_MASK		(1 << 10)
#define GEN2_STBY_MASK		(1 << 13)
#define PLL_STBY_MASK		(1 << 16)
#define USB2_STBY_MASK		(1 << 19)

#define GEN3_STBY_MASK		(1 << 1)
#define CAM_STBY_MASK		(1 << 7)
#define VIDEO_STBY_MASK		(1 << 13)
#define AUDIO_STBY_MASK		(1 << 16)
#define SD_STBY_MASK		(1 << 19)
#define VPLLSTBY_MASK		(1 << 16)
#define STANDBYSECINV_MASK	(1 << 11)
#define STANDBYINV_MASK		(1 << 10)
#define STANDBYINV_VALUE	(0 << 10)
#define VGEN2STBY_MASK		(1 << 13)
#define VUSB2STBY_MASK		(1 << 19)

#define SW_AUTO_MODE_MASK	0xf
#define SW_AUTO_MODE_VAL	0x8

/* Define WDIRESET and RESTARTEN */
#define RESTARTEN_LSH		0
#define WDIRESET_LSH		12

/* 0x92412 */
#define REG_MODE_0_ALL_MASK	(GEN1_STBY_MASK |\
				DIG_STBY_MASK)
/* 0x92082 */
#define REG_MODE_1_ALL_MASK	(CAM_STBY_MASK | VIDEO_STBY_MASK |\
				AUDIO_STBY_MASK | SD_STBY_MASK)

/* VGEN3 */
#define VGEN3CONFIG_SHIFT	3
#define VGEN3SETTING_SHIFT	14

/* switch mode setting */
#define	SW1MODE_LSB	0
#define	SW2MODE_LSB	10
#define	SW3MODE_LSB	0
#define	SW4MODE_LSB	8

#define	SWMODE_MASK	0xF
#define SWMODE_AUTO	0x8

#define PMIC_BUTTON_DEBOUNCE_MASK	0x3
#define PMIC_BUTTON_DEBOUNCE_VALUE	0x1

/* CPU */
static struct regulator_consumer_supply sw1_consumers[] = {
	{
		.supply = "cpu_vcc",
	}
};

static struct regulator_consumer_supply vgen1_consumers[] = {
	{
		/* sgtl5000 */
		.supply = "VDDA",
		.dev_name = "1-000a",
	},
	{
		/* sgtl5000 */
		.supply = "VDDIO",
		.dev_name = "1-000a",
	},
};

struct mc13892;

static struct regulator_init_data sw1_init = {
	.constraints = {
		.name = "SW1",
		.min_uV = mV_to_uV(600),
		.max_uV = mV_to_uV(1375),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.valid_modes_mask = 0,
		.always_on = 1,
		.boot_on = 1,
		.initial_state = PM_SUSPEND_MEM,
		.state_mem = {
			.uV = 850000,
			.mode = REGULATOR_MODE_NORMAL,
			.enabled = 1,
		},
	},
	.num_consumer_supplies = ARRAY_SIZE(sw1_consumers),
	.consumer_supplies = sw1_consumers,
};

/* For EVT2 devices */
static struct regulator_init_data sw1_init_evt2 = {
	.constraints = {
		.name = "SW1",
		.min_uV = mV_to_uV(600),
		.max_uV = mV_to_uV(1375),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.valid_modes_mask = 0,
		.always_on = 1,
		.boot_on = 1,
		.initial_state = PM_SUSPEND_MEM,
		.state_mem = {
			.uV = 850000,
			.mode = REGULATOR_MODE_NORMAL,
			.enabled = 1,
		},
	},
	.num_consumer_supplies = ARRAY_SIZE(sw1_consumers),
	.consumer_supplies = sw1_consumers,
};

static struct regulator_init_data sw2_init = {
	.constraints = {
		.name = "SW2",
		.min_uV = mV_to_uV(900),
		.max_uV = mV_to_uV(1850),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.always_on = 1,
		.boot_on = 1,
		.initial_state = PM_SUSPEND_MEM,
		.state_mem = {
			.uV = 1100000,
			.mode = REGULATOR_MODE_NORMAL,
			.enabled = 1,
		},
	}
};

static struct regulator_init_data sw2_init_evt2 = {
	.constraints = {
		.name = "SW2",
		.min_uV = mV_to_uV(900),
		.max_uV = mV_to_uV(1850),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.always_on = 1,
		.boot_on = 1,
		.initial_state = PM_SUSPEND_MEM,
		.state_mem = {
			.uV = 950000,
			.mode = REGULATOR_MODE_NORMAL,
			.enabled = 1,
		},
	}
};

static struct regulator_init_data sw3_init = {
	.constraints = {
		.name = "SW3",
		.min_uV = mV_to_uV(900),
		.max_uV = mV_to_uV(1850),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.always_on = 1,
		.boot_on = 1,
		.initial_state = PM_SUSPEND_MEM,
		.state_mem = {
			.uV = 950000,
			.mode = REGULATOR_MODE_NORMAL,
			.enabled = 1,
		},
	}
};

static struct regulator_init_data sw3_init_evt2 = {
	.constraints = {
		.name = "SW3",
		.min_uV = mV_to_uV(900),
		.max_uV = mV_to_uV(1850),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.always_on = 1,
		.boot_on = 1,
		.initial_state = PM_SUSPEND_MEM,
		.state_mem = {
			.uV = 950000,
			.mode = REGULATOR_MODE_NORMAL,
			.enabled = 1,
		},
	}
};

static struct regulator_init_data sw4_init = {
	.constraints = {
		.name = "SW4",
		.min_uV = mV_to_uV(1100),
		.max_uV = mV_to_uV(1850),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.always_on = 1,
		.boot_on = 1,
	}
};

static struct regulator_init_data viohi_init = {
	.constraints = {
		.name = "VIOHI",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.always_on = 1,
		.boot_on = 1,
	}
};

static struct regulator_init_data vusb_init = {
	.constraints = {
		.name = "VUSB",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.boot_on = 1,
		.always_on = 1,
	}
};

static struct regulator_init_data swbst_init = {
	.constraints = {
		.name = "SWBST",
		.boot_on = 1,
		.always_on = 0,
	}
};

static struct regulator_init_data vdig_init = {
	.constraints = {
		.name = "VDIG",
		.min_uV = mV_to_uV(1200),
		.max_uV = mV_to_uV(1200),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
		.boot_on = 1,
		.always_on = 1,
	},
};

static struct regulator_init_data vpll_init = {
	.constraints = {
		.name = "VPLL",
		.min_uV = mV_to_uV(1050),
		.max_uV = mV_to_uV(1800),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
		.boot_on = 1,
		.always_on = 1,
	}
};

static struct regulator_init_data vusb2_init = {
	.constraints = {
		.name = "VUSB2",
		.min_uV = mV_to_uV(2400),
		.max_uV = mV_to_uV(2775),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
		.boot_on = 1,
		.always_on = 1,
	}
};

static struct regulator_init_data vvideo_init = {
	.constraints = {
		.name = "VVIDEO",
		.min_uV = mV_to_uV(2775),
		.max_uV = mV_to_uV(2775),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.apply_uV = 1,
		.always_on = 0,
	},
};

static struct regulator_init_data vaudio_init = {
	.constraints = {
		.name = "VAUDIO",
		.min_uV = mV_to_uV(2300),
		.max_uV = mV_to_uV(3000),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE,
		.always_on = 0,
	}
};

static struct regulator_init_data vsd_init = {
	.constraints = {
		.name = "VSD",
		.min_uV = mV_to_uV(1800),
		.max_uV = mV_to_uV(3150),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
#ifdef CONFIG_MX50_YOSHI_SDCARD
		.always_on = 1,
#else
		.always_on = 0,
#endif
	}
};

static struct regulator_init_data vcam_init = {
	.constraints = {
		.name = "VCAM",
		.min_uV = mV_to_uV(2500),
		.max_uV = mV_to_uV(3000),
		.valid_ops_mask =
			REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_MODE | REGULATOR_CHANGE_STATUS,
		.valid_modes_mask = REGULATOR_MODE_FAST | REGULATOR_MODE_NORMAL,
		.always_on = 0,
	}
};

static struct regulator_init_data vgen1_init = {
	.constraints = {
		.name = "VGEN1",
		.min_uV = mV_to_uV(3000),
		.max_uV = mV_to_uV(3000),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
		.always_on = 0,
	},
	.num_consumer_supplies = ARRAY_SIZE(vgen1_consumers),
	.consumer_supplies = vgen1_consumers,
};

static struct regulator_init_data vgen2_init = {
	.constraints = {
		.name = "VGEN2",
		.min_uV = mV_to_uV(1200),
		.max_uV = mV_to_uV(3150),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
		.always_on = 1,
	}
};

static struct regulator_init_data vgen3_init = {
	.constraints = {
		.name = "VGEN3",
		.min_uV = mV_to_uV(1800),
		.max_uV = mV_to_uV(1800),
		.valid_ops_mask = REGULATOR_CHANGE_VOLTAGE | REGULATOR_CHANGE_STATUS,
		.always_on = 0,
	}
};

static struct regulator_init_data gpo1_init = {
	.constraints = {
		.name = "GPO1",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	}
};

static struct regulator_init_data gpo2_init = {
	.constraints = {
		.name = "GPO2",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	}
};

static struct regulator_init_data gpo3_init = {
	.constraints = {
		.name = "GPO3",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	}
};

static struct regulator_init_data gpo4_init = {
	.constraints = {
		.name = "GPO4",
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	}
};

static int mc13892_regulator_init(struct mc13892 *mc13892)
{
	unsigned int value, register_mask;
	printk(KERN_DEBUG "Initializing regulators for mx50 yoshi.\n");

	/* enable standby controll for all regulators */
	pmic_read_reg(REG_MODE_0, &value, 0xffffff);
	value |= REG_MODE_0_ALL_MASK;
	pmic_write_reg(REG_MODE_0, value, 0xffffff);

	pmic_read_reg(REG_MODE_1, &value, 0xffffff);
	value |= REG_MODE_1_ALL_MASK;
	pmic_write_reg(REG_MODE_1, value, 0xffffff);

	/* enable switch audo mode */
	pmic_read_reg(REG_IDENTIFICATION, &value, 0xffffff);
	/* only for mc13892 2.0A */
	if ((value & 0x0000FFFF) == 0x45d0) {
		pmic_read_reg(REG_SW_4, &value, 0xffffff);
		register_mask = (SWMODE_MASK << SW1MODE_LSB) |
		       (SWMODE_MASK << SW2MODE_LSB);
		value &= ~register_mask;
		value |= (SWMODE_AUTO << SW1MODE_LSB) |
			(SWMODE_AUTO << SW2MODE_LSB);
		pmic_write_reg(REG_SW_4, value, 0xffffff);

		pmic_read_reg(REG_SW_5, &value, 0xffffff);
		register_mask = (SWMODE_MASK << SW3MODE_LSB) |
			(SWMODE_MASK << SW4MODE_LSB);
		value &= ~register_mask;
		value |= (SWMODE_AUTO << SW3MODE_LSB) |
			(SWMODE_AUTO << SW4MODE_LSB);
		pmic_write_reg(REG_SW_5, value, 0xffffff);
	}
	/* Enable coin cell charger */
	value = BITFVAL(COINCHEN, 1) | BITFVAL(VCOIN, VCOIN_3_0V);
	register_mask = BITFMASK(COINCHEN) | BITFMASK(VCOIN);
	pmic_write_reg(REG_POWER_CTL0, value, register_mask);

#if defined(CONFIG_RTC_DRV_MXC_V2) || defined(CONFIG_RTC_DRV_MXC_V2_MODULE)
	value = BITFVAL(DRM, 1);
	register_mask = BITFMASK(DRM);
	pmic_write_reg(REG_POWER_CTL0, value, register_mask);
#endif

	if (mx50_board_rev_greater_eq(BOARD_ID_TEQUILA_EVT2) || 
		mx50_board_rev_greater_eq(BOARD_ID_WHITNEY_EVT2) ||
		mx50_board_rev_greater_eq(BOARD_ID_WHITNEY_WFO_EVT2) ||
		mx50_board_rev_greater_eq(BOARD_ID_CELESTE_EVT2_4) ||
		mx50_board_rev_greater_eq(BOARD_ID_CELESTE_WFO_EVT2_4) ||
		mx50_board_rev_greater_eq(BOARD_ID_CELESTE_256_EVT3) ||
		mx50_board_rev_greater_eq(BOARD_ID_CELESTE_WFO_512_EVT3)) {
		mc13892_register_regulator(mc13892, MC13892_SW1, &sw1_init_evt2);
		mc13892_register_regulator(mc13892, MC13892_SW2, &sw2_init_evt2);
		mc13892_register_regulator(mc13892, MC13892_SW3, &sw3_init_evt2);
	}
	else {
		mc13892_register_regulator(mc13892, MC13892_SW1, &sw1_init);
		mc13892_register_regulator(mc13892, MC13892_SW2, &sw2_init);
		mc13892_register_regulator(mc13892, MC13892_SW3, &sw3_init);
	}

	mc13892_register_regulator(mc13892, MC13892_SW4, &sw4_init);
	mc13892_register_regulator(mc13892, MC13892_SWBST, &swbst_init);
	mc13892_register_regulator(mc13892, MC13892_VIOHI, &viohi_init);
	mc13892_register_regulator(mc13892, MC13892_VPLL, &vpll_init);
	mc13892_register_regulator(mc13892, MC13892_VDIG, &vdig_init);
	mc13892_register_regulator(mc13892, MC13892_VSD, &vsd_init);
	mc13892_register_regulator(mc13892, MC13892_VUSB2, &vusb2_init);
	mc13892_register_regulator(mc13892, MC13892_VVIDEO, &vvideo_init);
	mc13892_register_regulator(mc13892, MC13892_VAUDIO, &vaudio_init);
	mc13892_register_regulator(mc13892, MC13892_VCAM, &vcam_init);
	mc13892_register_regulator(mc13892, MC13892_VGEN1, &vgen1_init);
	mc13892_register_regulator(mc13892, MC13892_VGEN2, &vgen2_init);
	mc13892_register_regulator(mc13892, MC13892_VGEN3, &vgen3_init);
	mc13892_register_regulator(mc13892, MC13892_VUSB, &vusb_init);
	mc13892_register_regulator(mc13892, MC13892_GPO1, &gpo1_init);
	mc13892_register_regulator(mc13892, MC13892_GPO2, &gpo2_init);
	mc13892_register_regulator(mc13892, MC13892_GPO3, &gpo3_init);
	mc13892_register_regulator(mc13892, MC13892_GPO4, &gpo4_init);

	/* Turn on VPLLSTBY */
	pmic_write_reg(REG_MODE_0, VPLLSTBY_MASK, VPLLSTBY_MASK);

	/* STANDBYINV set to 1 */
	pmic_write_reg(REG_POWER_CTL2, STANDBYINV_VALUE, STANDBYINV_MASK);

	regulator_has_full_constraints();

	/* SW4 - AUTO mode */
	pmic_write_reg(REG_SW_5, (SW_AUTO_MODE_VAL << 8), (SW_AUTO_MODE_MASK << 8));

	/* SW2 - AUTO mode */
	pmic_write_reg(REG_SW_4, (SW_AUTO_MODE_VAL << 10), (SW_AUTO_MODE_MASK << 10));

	/* SW3 - Auto mode */
	pmic_write_reg(REG_SW_5, (SW_AUTO_MODE_VAL << 0), (SW_AUTO_MODE_MASK << 0));
	
	/* SW1 - Auto mode */
	pmic_write_reg(REG_SW_4, (SW_AUTO_MODE_VAL << 0), (SW_AUTO_MODE_MASK << 0));

	/* VGEN3 */
	pmic_write_reg(REG_SETTING_0, (0 << VGEN3SETTING_SHIFT), (1 << VGEN3SETTING_SHIFT));
	pmic_write_reg(REG_MODE_1, (1 << VGEN3CONFIG_SHIFT), (1 << VGEN3CONFIG_SHIFT));

	/* configure the PMIC for WDIRESET */
	pmic_write_reg(REG_POWER_CTL2, (1 << WDIRESET_LSH), (1 << WDIRESET_LSH));

	/* Turn on RESTARTEN */
	pmic_write_reg(REG_POWER_CTL2, (1 << RESTARTEN_LSH), (1 << RESTARTEN_LSH));
	
	/* VGEN2 controlled by standby */
	pmic_write_reg(REG_MODE_0, VGEN2STBY_MASK, VGEN2STBY_MASK);

	/* set the debounce time on power buttons to 31.25ms */
	/* Configure debounce time for power button 1 */
	pmic_write_reg(REG_POWER_CTL2, (PMIC_BUTTON_DEBOUNCE_VALUE << 4),
			(PMIC_BUTTON_DEBOUNCE_MASK << 4));

	/* Configure debounce time for power button 2 */
	pmic_write_reg(REG_POWER_CTL2, (PMIC_BUTTON_DEBOUNCE_VALUE << 6),
			(PMIC_BUTTON_DEBOUNCE_MASK << 6));

	/* Configure debounce time for power button 3 */
	pmic_write_reg(REG_POWER_CTL2, (PMIC_BUTTON_DEBOUNCE_VALUE << 8),
			(PMIC_BUTTON_DEBOUNCE_MASK << 8));
	
	pmic_write_reg(REG_MODE_0, VUSB2STBY_MASK, VUSB2STBY_MASK);
	
	return 0;
}

static struct mc13892_platform_data mc13892_plat = {
	.init = mc13892_regulator_init,
};

static struct spi_board_info __initdata mc13892_spi_device = {
	.modalias = "pmic_spi",
	.irq = IOMUX_TO_IRQ(MX50_PIN_UART1_CTS),
	.max_speed_hz = 2000000,	/* max spi SCK clock speed in HZ */
	.bus_num = 3,
	.chip_select = 0,
	.platform_data = &mc13892_plat,
};

#define VUSB2EN_LSH	18
#define VUSB2_VOTE_SOURCE_OTG  (1U << 1)
#define VUSB2_VOTE_SOURCE_EHCI (1U << 2)
#define VUSB2_VOTE_SOURCE_CHARGER (1U << 3)
#define VUSB2_VOTE_ALL	(VUSB2_VOTE_SOURCE_OTG|VUSB2_VOTE_SOURCE_EHCI|VUSB2_VOTE_SOURCE_CHARGER)
void pmic_vusb2_enable_vote(int enable,int source)
{
	/* vbus2 is enabled on boot. OTG driver makes this assumption as well */
	static unsigned int vusb_enable = VUSB2_VOTE_SOURCE_OTG;

	pr_debug("%s: %d src=%d\n", __func__, enable, source);

	if ( enable ) {
		unsigned int was_enabled = 0;
		if( vusb_enable )
			was_enabled = 1;
		vusb_enable |= source;
		/* ignore all but the first vote for enable */
		if ( was_enabled )
			return;
	} else {
		/* it was disabled */
		if( !vusb_enable )
			return;

		vusb_enable &= ~source;
		/* only disable when the last client votes to disable */
		if( vusb_enable )
			return;
	}

	pr_debug("%s: %d set\n", __func__, enable);
	pmic_write_reg(REG_MODE_0, (enable << VUSB2EN_LSH), (1 << VUSB2EN_LSH));
	mdelay(10);
}
EXPORT_SYMBOL(pmic_vusb2_enable_vote);

int __init mx50_yoshi_init_mc13892(void)
{
	return spi_register_board_info(&mc13892_spi_device, 1);
}
