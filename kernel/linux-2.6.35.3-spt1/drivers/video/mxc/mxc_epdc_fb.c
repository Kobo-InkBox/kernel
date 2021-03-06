/*
 * Copyright (C) 2010-2011 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */
/*
 * Based on STMP378X LCDIF
 * Copyright 2008 Embedded Alley Solutions, Inc All Rights Reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/dmaengine.h>
#include <linux/pxp_dma.h>
#include <linux/mxcfb.h>
#include <linux/mxcfb_epdc_kernel.h>
#include <linux/gpio.h>
#include <linux/regulator/driver.h>
#include <linux/fsl_devices.h>
#ifdef CONFIG_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif

#include <linux/time.h>

#include "epdc_regs.h"

/*2011/2/17 FY11 : Supported to read waveform in eMMC. */
#include <linux/mmc/rawdatatable.h>

/* 2011/04/06 FY11 : Fixed the failure of waveform memory allocation. */
#include <linux/vmalloc.h>

/* 2011/04/19 FY11 : Supported wake lock. */
#include <linux/wakelock.h>

/*
 * Enable this define to have a default panel
 * loaded during driver initialization
 */
#define DEFAULT_PANEL_HW_INIT

#define NUM_SCREENS_MIN	2
#define EPDC_NUM_LUTS 16
#define EPDC_MAX_NUM_UPDATES 20
#define INVALID_LUT -1

#define DEFAULT_TEMP_INDEX	0
#define DEFAULT_TEMP		20 /* room temp in deg Celsius */

/* 2011/03/30 FY11 : Fixed tentative define. */
#define INIT_UPDATE_MARKER	(UPDATE_MARKER_MAX + 0x12345678)
#define PAN_UPDATE_MARKER	(UPDATE_MARKER_MAX + 0x12345679)
#define SSCREEN_UPDATE_MARKER	(UPDATE_MARKER_MAX + 0x1234567A)

#define POWER_STATE_OFF	0
#define POWER_STATE_ON	1


#if 1 /* E_BOOK */
/* 2010/12/10 FY11 : Added define for minus temperature. */
#define MINUS_TEMPERATURE	128
#define MINUS_TEMP_ADJUST_VAL	256
/* 2011/03/22 FY11 : Added retry to powerup. */
#define POWERUP_RETRY	1
/* 2011/07/13 FY11 : Modified default powerdown delay. (msec) */
#define DEFAULT_POWER_DOWN_DELAY	80

/* 2011/06/08 FY11 : Modified to use static buffer for waveform. */
#define WF_TMP_BUF_SIZE	4096
static u8 *wf_tmp_buf = NULL;
#endif /* E_BOOK */

#define MERGE_OK	0
#define MERGE_FAIL	1
#define MERGE_BLOCK	2

static unsigned long default_bpp = 16;

struct update_marker_data {
	u32 update_marker;
	struct completion update_completion;
	int lut_num;
};

/* This structure represents a list node containing both
 * a memory region allocated as an output buffer for the PxP
 * update processing task, and the update description (mode, region, etc.) */
struct update_data_list {
	struct list_head list;
	struct mxcfb_update_data upd_data;/* Update parameters */
	dma_addr_t phys_addr;		/* Pointer to phys address of processed Y buf */
	void *virt_addr;
	u32 epdc_offs;			/* Add to buffer pointer to resolve alignment */
	int lut_num;			/* Assigned before update is processed into working buffer */
	int collision_mask;		/* Set when update results in collision */
					/* Represents other LUTs that we collide with */
	struct update_marker_data *upd_marker_data;
	u32 update_order;		/* Numeric ordering value for update */
/* 2011/03/05 FY11 : Supported A2 mode limitations. */
	u32 use_white_buf;			/*  Flag for using white buffer. 0: not used, others: used */
/* 2011/03/15 FY11 : Supported standby screen. */
	u32 use_standbyscreen;			/*  Flag for using standby screen. 0: not used, others: used */
};

struct mxc_epdc_fb_data {
	struct fb_info info;
	struct fb_var_screeninfo epdc_fb_var; /* Internal copy of screeninfo
						so we can sync changes to it */
	u32 pseudo_palette[16];
	char fw_str[24];
	struct list_head list;
	struct mxc_epdc_fb_mode *cur_mode;
	struct mxc_epdc_fb_platform_data *pdata;
	int blank;
	u32 max_pix_size;
	ssize_t map_size;
	dma_addr_t phys_start;
	u32 fb_offset;
	int default_bpp;
	int native_width;
	int native_height;
	int num_screens;
	int epdc_irq;
	struct device *dev;
	int power_state;
	struct clk *epdc_clk_axi;
	struct clk *epdc_clk_pix;
	struct regulator *display_regulator;
	struct regulator *vcom_regulator;
	struct regulator *temp_regulator;
	struct regulator *epd_pwr0_regulator;
	struct regulator *epd_pwr2_regulator;
	struct regulator *v3p3_regulator;
/* 2011/07/05 FY11 : Added VSYS_EPD_ON. */
	struct regulator *vsys_regulator;
	bool fw_default_load;

	/* FB elements related to EPDC updates */
	bool in_init;
	bool hw_ready;
	bool waiting_for_idle;
	u32 auto_mode;
	u32 upd_scheme;
	struct update_data_list *upd_buf_queue;
	struct update_data_list *upd_buf_free_list;
	struct update_data_list *upd_buf_collision_list;
	struct update_data_list *cur_update;
	spinlock_t queue_lock;
	int trt_entries;
	int temp_index;
	u8 *temp_range_bounds;
	struct mxcfb_waveform_modes wv_modes;
	u32 *waveform_buffer_virt;
	u32 waveform_buffer_phys;
	u32 waveform_buffer_size;
	u32 *working_buffer_virt;
	u32 working_buffer_phys;
	u32 working_buffer_size;
	dma_addr_t phys_addr_copybuf;	/* Phys address of copied update data */
	void *virt_addr_copybuf;	/* Used for PxP SW workaround */
	u32 order_cnt;
	struct update_marker_data update_marker_array[EPDC_MAX_NUM_UPDATES];
	u32 lut_update_order[EPDC_NUM_LUTS];
	u32 luts_complete_wb;
	struct completion updates_done;
	struct delayed_work epdc_done_work;
	struct workqueue_struct *epdc_submit_workqueue;
	struct work_struct epdc_submit_work;
	bool waiting_for_wb;
	bool waiting_for_lut;
	bool waiting_for_lut15;
	struct completion update_res_free;
	struct completion lut15_free;
	struct completion eof_event;
	int eof_sync_period;
	struct mutex power_mutex;
	bool powering_down;
	int pwrdown_delay;
	unsigned long tce_prevent;

	/* FB elements related to PxP DMA */
	struct completion pxp_tx_cmpl;
	struct pxp_channel *pxp_chan;
	struct pxp_config_data pxp_conf;
	struct dma_async_tx_descriptor *txd;
	dma_cookie_t cookie;
	struct scatterlist sg[2];
	struct mutex pxp_mutex; /* protects access to PxP */

/* 2011/03/05 FY11 : Supported A2 mode limitations. */
	bool force_wf_mode;			/* Flag to represent wheather A2 mode is useded or not.  false : not used, true : used */

/* 2011/03/15 FY11 : Supported standby screen. */
	dma_addr_t standbyscreen_buff;	/* Physical address */
	char *standbyscreen_buff_virt;	/* Virtual address */
	ssize_t standbyscreen_buff_size;	/* Size of buffer */

/* 2011/04/19 FY11 : Supported wake lock. */
	struct wake_lock wake_lock_ioctl;	/* wake lock for ioctl */
	struct wake_lock wake_lock_update;	/* wake lock for update buffer */
	struct wake_lock wake_lock_power;	/* wake lock for EPD power */
	int counter_for_wlupdate;		/* counter of wake lock for update buffer */
/* 2011/07/14 FY11 : Added wake lock for A2. (Workaround for noise after sleep.) */
	struct wake_lock wake_lock_a2;	/* wake lock for A2 */

/* 2011/04/20 FY11 : Supported to clear panel when shutdown. */
	bool panel_clear_at_shutdown;

/* 2011/05/12 FY11 : Supported boot progress. */
	struct workqueue_struct *epdc_progress_work_queue;
	struct delayed_work epdc_progress_work;
};

struct waveform_data_header {
	unsigned int wi0;
	unsigned int wi1;
	unsigned int wi2;
	unsigned int wi3;
	unsigned int wi4;
	unsigned int wi5;
	unsigned int wi6;
	unsigned int xwia:24;
	unsigned int cs1:8;
	unsigned int wmta:24;
	unsigned int fvsn:8;
	unsigned int luts:8;
	unsigned int mc:8;
	unsigned int trc:8;
	unsigned int reserved0_0:8;
	unsigned int eb:8;
	unsigned int sb:8;
	unsigned int reserved0_1:8;
	unsigned int reserved0_2:8;
	unsigned int reserved0_3:8;
	unsigned int reserved0_4:8;
	unsigned int reserved0_5:8;
	unsigned int cs2:8;
};

struct mxcfb_waveform_data_file {
	struct waveform_data_header wdh;
	u32 *data;	/* Temperature Range Table + Waveform Data */
};

void __iomem *epdc_base;
static struct mxc_epdc_fb_data *epdc_fb_data;

struct mxc_epdc_fb_data *g_fb_data;

/* 2011/2/24 FY11 : Supported to read waveform version. */
#define WF_VER_OFFSET	0x0C
static struct mxcfb_waveform_version s_st_wfversion;


/* 2011/04/12 FY11 : Supported to write panel init flag. */
struct epd_settings {
	unsigned long wfsize;
	unsigned long vcom;
	unsigned long flag;
};

#define EPD_SETTING_PANEL_INIT  0x00000001



/* forward declaration */
static int mxc_epdc_fb_get_temp_index(struct mxc_epdc_fb_data *fb_data,
						int temp);
static void mxc_epdc_fb_flush_updates(struct mxc_epdc_fb_data *fb_data);
static int mxc_epdc_fb_blank(int blank, struct fb_info *info);
static int mxc_epdc_fb_init_hw(struct fb_info *info);
static int pxp_process_update(struct mxc_epdc_fb_data *fb_data,
			      u32 src_width, u32 src_height,
			      struct mxcfb_rect *update_region);
static int pxp_complete_update(struct mxc_epdc_fb_data *fb_data, u32 *hist_stat);

static void draw_mode0(struct mxc_epdc_fb_data *fb_data);
static bool is_free_list_full(struct mxc_epdc_fb_data *fb_data);
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
static int mxc_epdc_fb_load_wf( struct mxc_epdc_fb_data *fb_data, bool from_eMMC );


#ifdef CONFIG_SUB_MAIN_SPI
/* 2011/06/01 FY11 : Added watchdog timer. */
extern int sub_wdt_start(void);
extern int sub_wdt_stop(void);
extern int sub_wdt_clear(void);
#endif /* CONFIG_SUB_MAIN_SPI */

/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
#ifdef CONFIG_EPD_STATIC_MEM_WAVEFORM /* E_BOOK */
extern struct resource* get_res_epd_waveform(void);
#endif /* E_BOOK */
#ifdef CONFIG_EPD_STATIC_MEM_WORKBUFF /* E_BOOK */
extern struct resource* get_res_epd_workbuff(void);
#endif /* E_BOOK */

#define DEBUG

#ifdef DEBUG
static void dump_pxp_config(struct mxc_epdc_fb_data *fb_data,
			    struct pxp_config_data *pxp_conf)
{
	dev_err(fb_data->dev, "S0 fmt 0x%x",
		pxp_conf->s0_param.pixel_fmt);
	dev_err(fb_data->dev, "S0 width 0x%x",
		pxp_conf->s0_param.width);
	dev_err(fb_data->dev, "S0 height 0x%x",
		pxp_conf->s0_param.height);
	dev_err(fb_data->dev, "S0 ckey 0x%x",
		pxp_conf->s0_param.color_key);
	dev_err(fb_data->dev, "S0 ckey en 0x%x",
		pxp_conf->s0_param.color_key_enable);

	dev_err(fb_data->dev, "OL0 combine en 0x%x",
		pxp_conf->ol_param[0].combine_enable);
	dev_err(fb_data->dev, "OL0 fmt 0x%x",
		pxp_conf->ol_param[0].pixel_fmt);
	dev_err(fb_data->dev, "OL0 width 0x%x",
		pxp_conf->ol_param[0].width);
	dev_err(fb_data->dev, "OL0 height 0x%x",
		pxp_conf->ol_param[0].height);
	dev_err(fb_data->dev, "OL0 ckey 0x%x",
		pxp_conf->ol_param[0].color_key);
	dev_err(fb_data->dev, "OL0 ckey en 0x%x",
		pxp_conf->ol_param[0].color_key_enable);
	dev_err(fb_data->dev, "OL0 alpha 0x%x",
		pxp_conf->ol_param[0].global_alpha);
	dev_err(fb_data->dev, "OL0 alpha en 0x%x",
		pxp_conf->ol_param[0].global_alpha_enable);
	dev_err(fb_data->dev, "OL0 local alpha en 0x%x",
		pxp_conf->ol_param[0].local_alpha_enable);

	dev_err(fb_data->dev, "Out fmt 0x%x",
		pxp_conf->out_param.pixel_fmt);
	dev_err(fb_data->dev, "Out width 0x%x",
		pxp_conf->out_param.width);
	dev_err(fb_data->dev, "Out height 0x%x",
		pxp_conf->out_param.height);

	dev_err(fb_data->dev,
		"drect left 0x%x right 0x%x width 0x%x height 0x%x",
		pxp_conf->proc_data.drect.left, pxp_conf->proc_data.drect.top,
		pxp_conf->proc_data.drect.width,
		pxp_conf->proc_data.drect.height);
	dev_err(fb_data->dev,
		"srect left 0x%x right 0x%x width 0x%x height 0x%x",
		pxp_conf->proc_data.srect.left, pxp_conf->proc_data.srect.top,
		pxp_conf->proc_data.srect.width,
		pxp_conf->proc_data.srect.height);
	dev_err(fb_data->dev, "Scaling en 0x%x", pxp_conf->proc_data.scaling);
	dev_err(fb_data->dev, "HFlip en 0x%x", pxp_conf->proc_data.hflip);
	dev_err(fb_data->dev, "VFlip en 0x%x", pxp_conf->proc_data.vflip);
	dev_err(fb_data->dev, "Rotation 0x%x", pxp_conf->proc_data.rotate);
	dev_err(fb_data->dev, "BG Color 0x%x", pxp_conf->proc_data.bgcolor);
}

static void dump_epdc_reg(void)
{
	printk(KERN_ERR "\n\n");
	printk(KERN_ERR "EPDC_CTRL 0x%x\n", __raw_readl(EPDC_CTRL));
	printk(KERN_ERR "EPDC_WVADDR 0x%x\n", __raw_readl(EPDC_WVADDR));
	printk(KERN_ERR "EPDC_WB_ADDR 0x%x\n", __raw_readl(EPDC_WB_ADDR));
	printk(KERN_ERR "EPDC_RES 0x%x\n", __raw_readl(EPDC_RES));
	printk(KERN_ERR "EPDC_FORMAT 0x%x\n", __raw_readl(EPDC_FORMAT));
	printk(KERN_ERR "EPDC_FIFOCTRL 0x%x\n", __raw_readl(EPDC_FIFOCTRL));
	printk(KERN_ERR "EPDC_UPD_ADDR 0x%x\n", __raw_readl(EPDC_UPD_ADDR));
	printk(KERN_ERR "EPDC_UPD_FIXED 0x%x\n", __raw_readl(EPDC_UPD_FIXED));
	printk(KERN_ERR "EPDC_UPD_CORD 0x%x\n", __raw_readl(EPDC_UPD_CORD));
	printk(KERN_ERR "EPDC_UPD_SIZE 0x%x\n", __raw_readl(EPDC_UPD_SIZE));
	printk(KERN_ERR "EPDC_UPD_CTRL 0x%x\n", __raw_readl(EPDC_UPD_CTRL));
	printk(KERN_ERR "EPDC_TEMP 0x%x\n", __raw_readl(EPDC_TEMP));
	printk(KERN_ERR "EPDC_TCE_CTRL 0x%x\n", __raw_readl(EPDC_TCE_CTRL));
	printk(KERN_ERR "EPDC_TCE_SDCFG 0x%x\n", __raw_readl(EPDC_TCE_SDCFG));
	printk(KERN_ERR "EPDC_TCE_GDCFG 0x%x\n", __raw_readl(EPDC_TCE_GDCFG));
	printk(KERN_ERR "EPDC_TCE_HSCAN1 0x%x\n", __raw_readl(EPDC_TCE_HSCAN1));
	printk(KERN_ERR "EPDC_TCE_HSCAN2 0x%x\n", __raw_readl(EPDC_TCE_HSCAN2));
	printk(KERN_ERR "EPDC_TCE_VSCAN 0x%x\n", __raw_readl(EPDC_TCE_VSCAN));
	printk(KERN_ERR "EPDC_TCE_OE 0x%x\n", __raw_readl(EPDC_TCE_OE));
	printk(KERN_ERR "EPDC_TCE_POLARITY 0x%x\n", __raw_readl(EPDC_TCE_POLARITY));
	printk(KERN_ERR "EPDC_TCE_TIMING1 0x%x\n", __raw_readl(EPDC_TCE_TIMING1));
	printk(KERN_ERR "EPDC_TCE_TIMING2 0x%x\n", __raw_readl(EPDC_TCE_TIMING2));
	printk(KERN_ERR "EPDC_TCE_TIMING3 0x%x\n", __raw_readl(EPDC_TCE_TIMING3));
	printk(KERN_ERR "EPDC_IRQ_MASK 0x%x\n", __raw_readl(EPDC_IRQ_MASK));
	printk(KERN_ERR "EPDC_IRQ 0x%x\n", __raw_readl(EPDC_IRQ));
	printk(KERN_ERR "EPDC_STATUS_LUTS 0x%x\n", __raw_readl(EPDC_STATUS_LUTS));
	printk(KERN_ERR "EPDC_STATUS_NEXTLUT 0x%x\n", __raw_readl(EPDC_STATUS_NEXTLUT));
	printk(KERN_ERR "EPDC_STATUS_COL 0x%x\n", __raw_readl(EPDC_STATUS_COL));
	printk(KERN_ERR "EPDC_STATUS 0x%x\n", __raw_readl(EPDC_STATUS));
	printk(KERN_ERR "EPDC_DEBUG 0x%x\n", __raw_readl(EPDC_DEBUG));
	printk(KERN_ERR "EPDC_DEBUG_LUT0 0x%x\n", __raw_readl(EPDC_DEBUG_LUT0));
	printk(KERN_ERR "EPDC_DEBUG_LUT1 0x%x\n", __raw_readl(EPDC_DEBUG_LUT1));
	printk(KERN_ERR "EPDC_DEBUG_LUT2 0x%x\n", __raw_readl(EPDC_DEBUG_LUT2));
	printk(KERN_ERR "EPDC_DEBUG_LUT3 0x%x\n", __raw_readl(EPDC_DEBUG_LUT3));
	printk(KERN_ERR "EPDC_DEBUG_LUT4 0x%x\n", __raw_readl(EPDC_DEBUG_LUT4));
	printk(KERN_ERR "EPDC_DEBUG_LUT5 0x%x\n", __raw_readl(EPDC_DEBUG_LUT5));
	printk(KERN_ERR "EPDC_DEBUG_LUT6 0x%x\n", __raw_readl(EPDC_DEBUG_LUT6));
	printk(KERN_ERR "EPDC_DEBUG_LUT7 0x%x\n", __raw_readl(EPDC_DEBUG_LUT7));
	printk(KERN_ERR "EPDC_DEBUG_LUT8 0x%x\n", __raw_readl(EPDC_DEBUG_LUT8));
	printk(KERN_ERR "EPDC_DEBUG_LUT9 0x%x\n", __raw_readl(EPDC_DEBUG_LUT9));
	printk(KERN_ERR "EPDC_DEBUG_LUT10 0x%x\n", __raw_readl(EPDC_DEBUG_LUT10));
	printk(KERN_ERR "EPDC_DEBUG_LUT11 0x%x\n", __raw_readl(EPDC_DEBUG_LUT11));
	printk(KERN_ERR "EPDC_DEBUG_LUT12 0x%x\n", __raw_readl(EPDC_DEBUG_LUT12));
	printk(KERN_ERR "EPDC_DEBUG_LUT13 0x%x\n", __raw_readl(EPDC_DEBUG_LUT13));
	printk(KERN_ERR "EPDC_DEBUG_LUT14 0x%x\n", __raw_readl(EPDC_DEBUG_LUT14));
	printk(KERN_ERR "EPDC_DEBUG_LUT15 0x%x\n", __raw_readl(EPDC_DEBUG_LUT15));
	printk(KERN_ERR "EPDC_GPIO 0x%x\n", __raw_readl(EPDC_GPIO));
	printk(KERN_ERR "EPDC_VERSION 0x%x\n", __raw_readl(EPDC_VERSION));
	printk(KERN_ERR "\n\n");
}

static void dump_update_data(struct device *dev,
			     struct update_data_list *upd_data_list)
{
	dev_err(dev,
		"X = %d, Y = %d, Width = %d, Height = %d, WaveMode = %d, UpdateMode = %d, Flag = %d, "
		"LUT = %d, Coll Mask = 0x%x, order = %d\n",
		upd_data_list->upd_data.update_region.left,
		upd_data_list->upd_data.update_region.top,
		upd_data_list->upd_data.update_region.width,
		upd_data_list->upd_data.update_region.height,
		upd_data_list->upd_data.waveform_mode, 
		upd_data_list->upd_data.update_mode,
		upd_data_list->upd_data.flags,
		upd_data_list->lut_num,
		upd_data_list->collision_mask,
		upd_data_list->update_order);
}

static void dump_collision_list(struct mxc_epdc_fb_data *fb_data)
{
	struct update_data_list *plist;

	dev_err(fb_data->dev, "Collision List:\n");
	if (list_empty(&fb_data->upd_buf_collision_list->list))
		dev_err(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_buf_collision_list->list, list) {
		dev_err(fb_data->dev, "Virt Addr = 0x%x, Phys Addr = 0x%x ",
			(u32)plist->virt_addr, plist->phys_addr);
		dump_update_data(fb_data->dev, plist);
	}
}

static void dump_free_list(struct mxc_epdc_fb_data *fb_data)
{
	struct update_data_list *plist;

	dev_err(fb_data->dev, "Free List:\n");
	if (list_empty(&fb_data->upd_buf_free_list->list))
		dev_err(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_buf_free_list->list, list) {
		dev_err(fb_data->dev, "Virt Addr = 0x%x, Phys Addr = 0x%x ",
			(u32)plist->virt_addr, plist->phys_addr);
		dump_update_data(fb_data->dev, plist);
	}
}

static void dump_queue(struct mxc_epdc_fb_data *fb_data)
{
	struct update_data_list *plist;

	dev_err(fb_data->dev, "Queue:\n");
	if (list_empty(&fb_data->upd_buf_queue->list))
		dev_err(fb_data->dev, "Empty");
	list_for_each_entry(plist, &fb_data->upd_buf_queue->list, list) {
		dev_err(fb_data->dev, "Virt Addr = 0x%x, Phys Addr = 0x%x ",
			(u32)plist->virt_addr, plist->phys_addr);
		dump_update_data(fb_data->dev, plist);
	}
}

static void dump_all_updates(struct mxc_epdc_fb_data *fb_data)
{
	dump_free_list(fb_data);
	dump_queue(fb_data);
	dump_collision_list(fb_data);
	dev_err(fb_data->dev, "Current update being processed:\n");
	if (fb_data->cur_update == NULL)
		dev_err(fb_data->dev, "No current update\n");
	else
		dump_update_data(fb_data->dev, fb_data->cur_update);
}
#else
static inline void dump_pxp_config(struct mxc_epdc_fb_data *fb_data,
				   struct pxp_config_data *pxp_conf) {}
static inline void dump_epdc_reg(void) {}
static inline void dump_update_data(struct device *dev,
			     struct update_data_list *upd_data_list) {}
static inline void dump_collision_list(struct mxc_epdc_fb_data *fb_data) {}
static inline void dump_free_list(struct mxc_epdc_fb_data *fb_data) {}
static inline void dump_queue(struct mxc_epdc_fb_data *fb_data) {}
static inline void dump_all_updates(struct mxc_epdc_fb_data *fb_data) {}

#endif

static  ssize_t store_epdc_debug(struct device *device,
				struct device_attribute *attr,
				const char *buf, size_t count);
static ssize_t show_epdc_debug(struct device *device,
				struct device_attribute *attr, char *buf);
static struct device_attribute epdc_debug_attr = __ATTR(epdc_debug, S_IRUGO|S_IWUSR, show_epdc_debug, store_epdc_debug);
static u32 epdc_debug = 0;
#define epdc_debug_printk(fmt, args...)	if (epdc_debug) printk(KERN_ERR "%s: " fmt, __func__, ## args)


/********************************************************
 * Start Low-Level EPDC Functions
 ********************************************************/
static  ssize_t store_epdc_debug(struct device *device,
				struct device_attribute *attr,
				const char *buf, size_t count)
{               
        u32 state;
        char *last = NULL;

	state = simple_strtoul(buf, &last, 0);
	printk( KERN_ERR "%s val = %u\n", __func__, state );
	if ( state < 2 )
	{
		epdc_debug = state;
	}

	return count;
}

static int epdc_clock_gating ( bool gating );
static ssize_t show_epdc_debug(struct device *device,
				struct device_attribute *attr, char *buf)
{
	if ( epdc_fb_data )
	{
		unsigned long flags;
		clk_enable(epdc_fb_data->epdc_clk_axi);
		epdc_clock_gating(false);       // enable clock
		spin_lock_irqsave(&(epdc_fb_data->queue_lock), flags);
		dump_epdc_reg();
		dump_all_updates(epdc_fb_data);
		spin_unlock_irqrestore(&(epdc_fb_data->queue_lock), flags);
		epdc_clock_gating(true);
		clk_disable(epdc_fb_data->epdc_clk_axi);
	}

        return snprintf(buf, PAGE_SIZE, "%u\n", epdc_debug);
}


/* 2011/06/14 FY11 : Modified to handle epdc clock access count. */
static int epdc_clock_gate_cnt = 0;
static int epdc_clock_gating ( bool gating )
{
	int ret = 0;

	if ( gating )
	{
		if ( epdc_clock_gate_cnt == 0 )
		{
			printk (KERN_ERR "%s Invalid gate count!!\n", __func__ );
		}
		else
		{
			epdc_clock_gate_cnt--;
			epdc_debug_printk( " gate cnt down (%d)\n", epdc_clock_gate_cnt );
			if ( epdc_clock_gate_cnt == 0 )
			{
				__raw_writel(EPDC_CTRL_CLKGATE, EPDC_CTRL_SET);
			}
		}
	}
	else
	{
		if ( epdc_clock_gate_cnt == 0 )
		{
			__raw_writel(EPDC_CTRL_CLKGATE, EPDC_CTRL_CLEAR);
		}
		epdc_clock_gate_cnt++;
		epdc_debug_printk( " gate cnt up (%d)\n", epdc_clock_gate_cnt );
	}

	return ret;
}


static inline void epdc_lut_complete_intr(u32 lut_num, bool enable)
{
	if (enable)
		__raw_writel(1 << lut_num, EPDC_IRQ_MASK_SET);
	else
		__raw_writel(1 << lut_num, EPDC_IRQ_MASK_CLEAR);
}

static inline void epdc_working_buf_intr(bool enable)
{
	if (enable)
		__raw_writel(EPDC_IRQ_WB_CMPLT_IRQ, EPDC_IRQ_MASK_SET);
	else
		__raw_writel(EPDC_IRQ_WB_CMPLT_IRQ, EPDC_IRQ_MASK_CLEAR);
}

static inline void epdc_clear_working_buf_irq(void)
{
	__raw_writel(EPDC_IRQ_WB_CMPLT_IRQ | EPDC_IRQ_LUT_COL_IRQ,
		     EPDC_IRQ_CLEAR);
}

static inline void epdc_eof_intr(bool enable)
{
	if (enable)
		__raw_writel(EPDC_IRQ_FRAME_END_IRQ, EPDC_IRQ_MASK_SET);
	else
		__raw_writel(EPDC_IRQ_FRAME_END_IRQ, EPDC_IRQ_MASK_CLEAR);
}

static inline void epdc_clear_eof_irq(void)
{
	__raw_writel(EPDC_IRQ_FRAME_END_IRQ, EPDC_IRQ_CLEAR);
}

static inline bool epdc_signal_eof(void)
{
	return (__raw_readl(EPDC_IRQ_MASK) & __raw_readl(EPDC_IRQ)
		& EPDC_IRQ_FRAME_END_IRQ) ? true : false;
}


static inline void epdc_set_temp(u32 temp)
{
	__raw_writel(temp, EPDC_TEMP);
}

static inline void epdc_set_border_gpio(u32 mode)
{
	u32 val = ((__raw_readl(EPDC_GPIO) & ~EPDC_GPIO_BDR_MASK)| ((mode << EPDC_GPIO_BDR_OFFSET) & EPDC_GPIO_BDR_MASK));
	__raw_writel(val, EPDC_GPIO);
}

static inline void epdc_set_screen_res(u32 width, u32 height)
{
	u32 val = (height << EPDC_RES_VERTICAL_OFFSET) | width;
	__raw_writel(val, EPDC_RES);
}

static inline void epdc_set_update_addr(u32 addr)
{
	__raw_writel(addr, EPDC_UPD_ADDR);
}

static inline void epdc_set_update_coord(u32 x, u32 y)
{
	u32 val = (y << EPDC_UPD_CORD_YCORD_OFFSET) | x;
	__raw_writel(val, EPDC_UPD_CORD);
}

static inline void epdc_set_update_dimensions(u32 width, u32 height)
{
	u32 val = (height << EPDC_UPD_SIZE_HEIGHT_OFFSET) | width;
	__raw_writel(val, EPDC_UPD_SIZE);
}

static void epdc_submit_update(u32 lut_num, u32 waveform_mode, u32 update_mode,
			       bool use_test_mode, u32 np_val)
{
	u32 reg_val = 0;
	int ret = 0;

	if (use_test_mode) {
		reg_val |=
		    ((np_val << EPDC_UPD_FIXED_FIXNP_OFFSET) &
		     EPDC_UPD_FIXED_FIXNP_MASK) | EPDC_UPD_FIXED_FIXNP_EN;

		__raw_writel(reg_val, EPDC_UPD_FIXED);

		reg_val = EPDC_UPD_CTRL_USE_FIXED;
	} else {
		__raw_writel(reg_val, EPDC_UPD_FIXED);
	}

	reg_val |=
	    ((lut_num << EPDC_UPD_CTRL_LUT_SEL_OFFSET) &
	     EPDC_UPD_CTRL_LUT_SEL_MASK) |
	    ((waveform_mode << EPDC_UPD_CTRL_WAVEFORM_MODE_OFFSET) &
	     EPDC_UPD_CTRL_WAVEFORM_MODE_MASK) |
	    update_mode;

	__raw_writel(reg_val, EPDC_UPD_CTRL);


#ifdef CONFIG_SUB_MAIN_SPI
/* 2011/06/01 FY11 : Added watchdog timer. */
	epdc_debug_printk( " watchdog clear.\n" );
	ret = sub_wdt_clear();
	if ( ret < 0 )
	{
		printk(KERN_ERR "%s fale to clear watchdog. err = 0x%x\n", __func__, ret);
	}
#endif /* CONFIG_SUB_MAIN_SPI */
}

static inline bool epdc_is_lut_complete(u32 lut_num)
{
	u32 val = __raw_readl(EPDC_IRQ);
	bool is_compl = val & (1 << lut_num) ? true : false;

	return is_compl;
}

static inline void epdc_clear_lut_complete_irq(u32 lut_num)
{
	__raw_writel(1 << lut_num, EPDC_IRQ_CLEAR);
}

static inline bool epdc_is_lut_active(u32 lut_num)
{
	u32 val = __raw_readl(EPDC_STATUS_LUTS);
	bool is_active = val & (1 << lut_num) ? true : false;

	return is_active;
}

static inline bool epdc_any_luts_active(void)
{
	bool any_active = __raw_readl(EPDC_STATUS_LUTS) ? true : false;

	return any_active;
}

static inline bool epdc_any_luts_available(void)
{
	bool luts_available =
	    (__raw_readl(EPDC_STATUS_NEXTLUT) &
	     EPDC_STATUS_NEXTLUT_NEXT_LUT_VALID) ? true : false;
	return luts_available;
}

static inline int epdc_get_next_lut(void)
{
	u32 val =
	    __raw_readl(EPDC_STATUS_NEXTLUT) &
	    EPDC_STATUS_NEXTLUT_NEXT_LUT_MASK;
	return val;
}

static int epdc_choose_next_lut(int *next_lut)
{
	u32 luts_status = __raw_readl(EPDC_STATUS_LUTS);

	*next_lut = 32 - __builtin_clz(luts_status & 0xFFFF);

	if (*next_lut > 15)
		*next_lut = ffz(luts_status & 0xFFFF);

	if (luts_status & 0x8000)
		return 1;
	else
		return 0;
}


static inline bool epdc_is_working_buffer_busy(void)
{
	u32 val = __raw_readl(EPDC_STATUS);
	bool is_busy = (val & EPDC_STATUS_WB_BUSY) ? true : false;

	return is_busy;
}

static inline bool epdc_is_working_buffer_complete(void)
{
	u32 val = __raw_readl(EPDC_IRQ);
	bool is_compl = (val & EPDC_IRQ_WB_CMPLT_IRQ) ? true : false;

	return is_compl;
}

static inline bool epdc_is_collision(void)
{
	u32 val = __raw_readl(EPDC_IRQ);
	return (val & EPDC_IRQ_LUT_COL_IRQ) ? true : false;
}

static inline int epdc_get_colliding_luts(void)
{
	u32 val = __raw_readl(EPDC_STATUS_COL);
	return val;
}

static void epdc_set_horizontal_timing(u32 horiz_start, u32 horiz_end,
				       u32 hsync_width, u32 hsync_line_length)
{
	u32 reg_val =
	    ((hsync_width << EPDC_TCE_HSCAN1_LINE_SYNC_WIDTH_OFFSET) &
	     EPDC_TCE_HSCAN1_LINE_SYNC_WIDTH_MASK)
	    | ((hsync_line_length << EPDC_TCE_HSCAN1_LINE_SYNC_OFFSET) &
	       EPDC_TCE_HSCAN1_LINE_SYNC_MASK);
	__raw_writel(reg_val, EPDC_TCE_HSCAN1);

	reg_val =
	    ((horiz_start << EPDC_TCE_HSCAN2_LINE_BEGIN_OFFSET) &
	     EPDC_TCE_HSCAN2_LINE_BEGIN_MASK)
	    | ((horiz_end << EPDC_TCE_HSCAN2_LINE_END_OFFSET) &
	       EPDC_TCE_HSCAN2_LINE_END_MASK);
	__raw_writel(reg_val, EPDC_TCE_HSCAN2);
}

static void epdc_set_vertical_timing(u32 vert_start, u32 vert_end,
				     u32 vsync_width)
{
	u32 reg_val =
	    ((vert_start << EPDC_TCE_VSCAN_FRAME_BEGIN_OFFSET) &
	     EPDC_TCE_VSCAN_FRAME_BEGIN_MASK)
	    | ((vert_end << EPDC_TCE_VSCAN_FRAME_END_OFFSET) &
	       EPDC_TCE_VSCAN_FRAME_END_MASK)
	    | ((vsync_width << EPDC_TCE_VSCAN_FRAME_SYNC_OFFSET) &
	       EPDC_TCE_VSCAN_FRAME_SYNC_MASK);
	__raw_writel(reg_val, EPDC_TCE_VSCAN);
}

void epdc_init_settings(struct mxc_epdc_fb_data *fb_data)
{
	struct mxc_epdc_fb_mode *epdc_mode = fb_data->cur_mode;
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 reg_val;
	int num_ce;

	/* Reset */
	__raw_writel(EPDC_CTRL_SFTRST, EPDC_CTRL_SET);
	while (!(__raw_readl(EPDC_CTRL) & EPDC_CTRL_CLKGATE))
		;
	__raw_writel(EPDC_CTRL_SFTRST, EPDC_CTRL_CLEAR);

	/* Enable clock gating (clear to enable) */
	__raw_writel(EPDC_CTRL_CLKGATE, EPDC_CTRL_CLEAR);
	while (__raw_readl(EPDC_CTRL) & (EPDC_CTRL_SFTRST | EPDC_CTRL_CLKGATE))
		;

	/* EPDC_CTRL */
	reg_val = __raw_readl(EPDC_CTRL);
	reg_val &= ~EPDC_CTRL_UPD_DATA_SWIZZLE_MASK;
	reg_val |= EPDC_CTRL_UPD_DATA_SWIZZLE_NO_SWAP;
	reg_val &= ~EPDC_CTRL_LUT_DATA_SWIZZLE_MASK;
	reg_val |= EPDC_CTRL_LUT_DATA_SWIZZLE_NO_SWAP;
	__raw_writel(reg_val, EPDC_CTRL_SET);

	/* EPDC_FORMAT - 2bit TFT and 4bit Buf pixel format */
	reg_val = EPDC_FORMAT_TFT_PIXEL_FORMAT_2BIT
	    | EPDC_FORMAT_BUF_PIXEL_FORMAT_P4N
	    | ((0x0 << EPDC_FORMAT_DEFAULT_TFT_PIXEL_OFFSET) &
	       EPDC_FORMAT_DEFAULT_TFT_PIXEL_MASK);
	__raw_writel(reg_val, EPDC_FORMAT);

	/* EPDC_FIFOCTRL (disabled) */
	reg_val =
	    ((100 << EPDC_FIFOCTRL_FIFO_INIT_LEVEL_OFFSET) &
	     EPDC_FIFOCTRL_FIFO_INIT_LEVEL_MASK)
	    | ((200 << EPDC_FIFOCTRL_FIFO_H_LEVEL_OFFSET) &
	       EPDC_FIFOCTRL_FIFO_H_LEVEL_MASK)
	    | ((100 << EPDC_FIFOCTRL_FIFO_L_LEVEL_OFFSET) &
	       EPDC_FIFOCTRL_FIFO_L_LEVEL_MASK);
	__raw_writel(reg_val, EPDC_FIFOCTRL);

	/* EPDC_TEMP - Use default temp to get index */
	epdc_set_temp(mxc_epdc_fb_get_temp_index(fb_data, DEFAULT_TEMP));

	/* EPDC_RES */
	epdc_set_screen_res(epdc_mode->vmode->xres, epdc_mode->vmode->yres);

	/*
	 * EPDC_TCE_CTRL
	 * VSCAN_HOLDOFF = 4
	 * VCOM_MODE = MANUAL
	 * VCOM_VAL = 0
	 * DDR_MODE = DISABLED
	 * LVDS_MODE_CE = DISABLED
	 * LVDS_MODE = DISABLED
	 * DUAL_SCAN = DISABLED
	 * SDDO_WIDTH = 8bit
	 * PIXELS_PER_SDCLK = 4
	 */
	reg_val =
	    ((epdc_mode->vscan_holdoff << EPDC_TCE_CTRL_VSCAN_HOLDOFF_OFFSET) &
	     EPDC_TCE_CTRL_VSCAN_HOLDOFF_MASK)
	    | EPDC_TCE_CTRL_PIXELS_PER_SDCLK_4;
	__raw_writel(reg_val, EPDC_TCE_CTRL);

	/* EPDC_TCE_HSCAN */
	epdc_set_horizontal_timing(screeninfo->left_margin,
				   screeninfo->right_margin,
				   screeninfo->hsync_len,
				   screeninfo->hsync_len);

	/* EPDC_TCE_VSCAN */
	epdc_set_vertical_timing(screeninfo->upper_margin,
				 screeninfo->lower_margin,
				 screeninfo->vsync_len);

	/* EPDC_TCE_OE */
	reg_val =
	    ((epdc_mode->sdoed_width << EPDC_TCE_OE_SDOED_WIDTH_OFFSET) &
	     EPDC_TCE_OE_SDOED_WIDTH_MASK)
	    | ((epdc_mode->sdoed_delay << EPDC_TCE_OE_SDOED_DLY_OFFSET) &
	       EPDC_TCE_OE_SDOED_DLY_MASK)
	    | ((epdc_mode->sdoez_width << EPDC_TCE_OE_SDOEZ_WIDTH_OFFSET) &
	       EPDC_TCE_OE_SDOEZ_WIDTH_MASK)
	    | ((epdc_mode->sdoez_delay << EPDC_TCE_OE_SDOEZ_DLY_OFFSET) &
	       EPDC_TCE_OE_SDOEZ_DLY_MASK);
	__raw_writel(reg_val, EPDC_TCE_OE);

	/* EPDC_TCE_TIMING1 */
	__raw_writel(0x0, EPDC_TCE_TIMING1);

	/* EPDC_TCE_TIMING2 */
	reg_val =
	    ((epdc_mode->gdclk_hp_offs << EPDC_TCE_TIMING2_GDCLK_HP_OFFSET) &
	     EPDC_TCE_TIMING2_GDCLK_HP_MASK)
	    | ((epdc_mode->gdsp_offs << EPDC_TCE_TIMING2_GDSP_OFFSET_OFFSET) &
	       EPDC_TCE_TIMING2_GDSP_OFFSET_MASK);
	__raw_writel(reg_val, EPDC_TCE_TIMING2);

	/* EPDC_TCE_TIMING3 */
	reg_val =
	    ((epdc_mode->gdoe_offs << EPDC_TCE_TIMING3_GDOE_OFFSET_OFFSET) &
	     EPDC_TCE_TIMING3_GDOE_OFFSET_MASK)
	    | ((epdc_mode->gdclk_offs << EPDC_TCE_TIMING3_GDCLK_OFFSET_OFFSET) &
	       EPDC_TCE_TIMING3_GDCLK_OFFSET_MASK);
	__raw_writel(reg_val, EPDC_TCE_TIMING3);

	/*
	 * EPDC_TCE_SDCFG
	 * SDCLK_HOLD = 1
	 * SDSHR = 1
	 * NUM_CE = 1
	 * SDDO_REFORMAT = FLIP_PIXELS
	 * SDDO_INVERT = DISABLED
	 * PIXELS_PER_CE = display horizontal resolution
	 */
	num_ce = epdc_mode->num_ce;
	if (num_ce == 0)
		num_ce = 1;
	reg_val = EPDC_TCE_SDCFG_SDCLK_HOLD | EPDC_TCE_SDCFG_SDSHR
	    | ((num_ce << EPDC_TCE_SDCFG_NUM_CE_OFFSET) &
	       EPDC_TCE_SDCFG_NUM_CE_MASK)
	    | EPDC_TCE_SDCFG_SDDO_REFORMAT_FLIP_PIXELS
	    | ((epdc_mode->vmode->xres/num_ce << EPDC_TCE_SDCFG_PIXELS_PER_CE_OFFSET) &
	       EPDC_TCE_SDCFG_PIXELS_PER_CE_MASK);
	__raw_writel(reg_val, EPDC_TCE_SDCFG);

	/*
	 * EPDC_TCE_GDCFG
	 * GDRL = 1
	 * GDOE_MODE = 0;
	 * GDSP_MODE = 0;
	 */
	reg_val = EPDC_TCE_SDCFG_GDRL;
	__raw_writel(reg_val, EPDC_TCE_GDCFG);

	/*
	 * EPDC_TCE_POLARITY
	 * SDCE_POL = ACTIVE LOW
	 * SDLE_POL = ACTIVE HIGH
	 * SDOE_POL = ACTIVE HIGH
	 * GDOE_POL = ACTIVE HIGH
	 * GDSP_POL = ACTIVE LOW
	 */
	reg_val = EPDC_TCE_POLARITY_SDLE_POL_ACTIVE_HIGH
	    | EPDC_TCE_POLARITY_SDOE_POL_ACTIVE_HIGH
	    | EPDC_TCE_POLARITY_GDOE_POL_ACTIVE_HIGH;
	__raw_writel(reg_val, EPDC_TCE_POLARITY);

	/* EPDC_IRQ_MASK */
	__raw_writel(EPDC_IRQ_TCE_UNDERRUN_IRQ, EPDC_IRQ_MASK);

	/*
	 * EPDC_GPIO
	 * PWRCOM = ?
	 * PWRCTRL = ?
	 * BDR = ?
	 */
	reg_val = ((0 << EPDC_GPIO_PWRCTRL_OFFSET) & EPDC_GPIO_PWRCTRL_MASK)
	    | ((0 << EPDC_GPIO_BDR_OFFSET) & EPDC_GPIO_BDR_MASK);
	__raw_writel(reg_val, EPDC_GPIO);
}

/* 2011/06/29 FY11 : Added workaround for too hot EPD PMIC. */
static int epdc_powerup_err_cnt = 0;
#define EPDC_PROHIBITION_THRESH	10

/* 2011/03/09 FY11 : Fixed the bug that CPU hangs caused by accessing to EPDC without clock. */
static int epdc_powerup(struct mxc_epdc_fb_data *fb_data)
{
	int ret = 0;
/* 2011/03/22 FY11 : Added retry to powerup. */
	int retry = POWERUP_RETRY;
	mutex_lock(&fb_data->power_mutex);

/* 2011/06/29 FY11 : Added workaround for too hot EPD PMIC. */
	if ( epdc_powerup_err_cnt >= EPDC_PROHIBITION_THRESH )
	{
		ret = -EIO;
		mutex_unlock(&fb_data->power_mutex);
/* 2011/07/05 FY11 : Added VSYS disable. */
		regulator_disable(fb_data->vsys_regulator);
		printk( "%s : The use of EPD PMIC is prohibited.\n", __func__ );
		return ret;
	}

	/*
	 * If power down request is pending, clear
	 * powering_down to cancel the request.
	 */
	if (fb_data->powering_down)
	{
/* 2011/07/12 FY11 : Added cancel scheduled powerdown. */
		cancel_delayed_work( &(fb_data->epdc_done_work) );
		fb_data->powering_down = false;
	}

	if (fb_data->power_state == POWER_STATE_ON) {
		mutex_unlock(&fb_data->power_mutex);
		epdc_debug_printk( " power is already on.\n" );
		return ret;
	}

	dev_dbg(fb_data->dev, "EPDC Powerup\n");
	epdc_debug_printk( " \n" );

/* 2011/03/22 FY11 : Added retry to powerup. */
retry_start:
	/* Enable power to the EPD panel */
	ret = regulator_enable(fb_data->display_regulator);
	if ( ret < 0 ) {
		dev_err(fb_data->dev, "Unable to enable DISPLAY regulator."
			"err = 0x%x\n", ret);
		goto err_out2;
	}

	ret = regulator_enable(fb_data->v3p3_regulator);
	if ( ret < 0 )
	{
		printk (KERN_ERR "V3P3 regulator_enable failed. %d\n", ret );
		goto err_out2;
	}

	/* Enable pins used by EPDC */
	if (fb_data->pdata->enable_pins)
		fb_data->pdata->enable_pins();

	/* Enable clocks to EPDC */
	clk_enable(fb_data->epdc_clk_axi);
	clk_enable(fb_data->epdc_clk_pix);

/* 2011/06/14 FY11 : Modified to handle epdc clock access count. */
	epdc_clock_gating(false);	// enable clock


	ret = regulator_enable(fb_data->epd_pwr0_regulator);
	if ( ret < 0 ) {
		dev_err(fb_data->dev, "Unable to enable PWR0 regulator."
			"err = 0x%x\n", ret);
		goto err_out3;
	}
	ret = regulator_enable(fb_data->vcom_regulator);
	if ( ret < 0 ) {
		dev_err(fb_data->dev, "Unable to enable VCOM regulator."
			"err = 0x%x\n", ret);
		goto err_out4;
	}

	fb_data->power_state = POWER_STATE_ON;
#ifdef CONFIG_SUB_MAIN_SPI
/* 2011/06/01 FY11 : Added watchdog timer. */
	epdc_debug_printk( " watchdog start.\n" );
	ret = sub_wdt_start();
	if ( ret < 0 )
	{
		dev_err(fb_data->dev, "fale to start watchdog. "
			"err = 0x%x\n", ret);
		goto err_out4;
	}
#endif /* CONFIG_SUB_MAIN_SPI */

/* 2011/04/19 FY11 : Supported wake lock. */
	epdc_debug_printk( "lock for power\n" ); 
	wake_lock( &(fb_data->wake_lock_power) );
/* 2011/06/29 FY11 : Added workaround for too hot EPD PMIC. */
	epdc_powerup_err_cnt = 0;

	goto out1;


err_out4:
	regulator_disable(fb_data->epd_pwr0_regulator);

err_out3:
	regulator_disable(fb_data->v3p3_regulator);
	/* Disable clocks to EPDC */
/* 2011/06/14 FY11 : Modified to handle epdc clock access count. */
	epdc_clock_gating(true);
	clk_disable(fb_data->epdc_clk_pix);
	clk_disable(fb_data->epdc_clk_axi);
	/* Disable pins used by EPDC (to prevent leakage current) */
	if (fb_data->pdata->disable_pins)
		fb_data->pdata->disable_pins();
	
err_out2:
	regulator_disable(fb_data->display_regulator);
/* 2011/03/22 FY11 : Added retry to powerup. */
	if ( retry > 0 )
	{
		retry--;
		msleep(1);
		printk(KERN_ERR "powerup retry.\n" );
		goto retry_start;
	}
	else
	{
/* 2011/06/29 FY11 : Added workaround for too hot EPD PMIC. */
		epdc_powerup_err_cnt++;
		printk(KERN_ERR "powerup retry out. (powerup error count: %d\n", epdc_powerup_err_cnt );
	}

out1:
	mutex_unlock(&fb_data->power_mutex);

	return ret;
}

static void epdc_powerdown(struct mxc_epdc_fb_data *fb_data)
{
	int ret = 0;

	mutex_lock(&fb_data->power_mutex);

	/* If powering_down has been cleared, a powerup
	 * request is pre-empting this powerdown request.
	 */
	if (!fb_data->powering_down
		|| (fb_data->power_state == POWER_STATE_OFF)) {
		mutex_unlock(&fb_data->power_mutex);
		fb_data->powering_down = false;
		epdc_debug_printk( " power is already off.\n" );
		return;
	}

	dev_dbg(fb_data->dev, "EPDC Powerdown\n");
	epdc_debug_printk( " \n" );

/* 2011/07/05 FY11 : Added workaround for EPD PMIC heat up. */
	/* Check power stat */
	if ( ! regulator_is_enabled( fb_data->epd_pwr0_regulator ) )
	{
		/* Never turn on epd pmic and disable VSYS_EPD later. */
		epdc_powerup_err_cnt = EPDC_PROHIBITION_THRESH;
	}

	/* Disable clocks to EPDC */
/* 2011/06/14 FY11 : Modified to handle epdc clock access count. */
	epdc_clock_gating(true);
	clk_disable(fb_data->epdc_clk_pix);
	clk_disable(fb_data->epdc_clk_axi);

	/* Disable pins used by EPDC (to prevent leakage current) */
	if (fb_data->pdata->disable_pins)
		fb_data->pdata->disable_pins();

	/* Disable power to the EPD panel */
	regulator_disable(fb_data->vcom_regulator);
	regulator_disable(fb_data->epd_pwr0_regulator);
	regulator_disable(fb_data->v3p3_regulator);
	regulator_disable(fb_data->display_regulator);

	fb_data->power_state = POWER_STATE_OFF;
	fb_data->powering_down = false;

#ifdef CONFIG_SUB_MAIN_SPI
/* 2011/06/01 FY11 : Added watchdog timer. */
	epdc_debug_printk( " watchdog stop.\n" );
	ret = sub_wdt_stop();
	if ( ret < 0 )
	{
		dev_err(fb_data->dev, "fale to stop watchdog. "
			"err = 0x%x\n", ret);
	}
#endif /* CONFIG_SUB_MAIN_SPI */

/* 2011/04/19 FY11 : Supported wake lock. */
	wake_unlock( &(fb_data->wake_lock_power) );
	epdc_debug_printk( "unlock for power\n" ); 

	mutex_unlock(&fb_data->power_mutex);

/* 2011/07/05 FY11 : Added workaround for EPD PMIC heat up. */
	if ( epdc_powerup_err_cnt >= EPDC_PROHIBITION_THRESH )
	{
		dev_err(fb_data->dev, "EPD PMIC is in abnormal condition.\n");
		regulator_disable(fb_data->vsys_regulator);
	}
}


/* 2011/05/12 FY11 : Supported boot progress. */
int mxc_epdc_fb_read_temperature(struct mxc_epdc_fb_data *fb_data);
static int epdc_draw_rect( struct mxc_epdc_fb_data *fb_data, 
			   struct mxcfb_update_data *updata,
			   u32 val,
			   bool wait_update )
{
	int ret = 0, lut, retry;

	retry = 60;
	while ( retry > 0 )
	{
		if ( epdc_any_luts_available() )
		{
			break;
		}
		retry--;
		msleep( 50 );
	}

	if ( retry == 0 )
	{
		printk( KERN_ERR "%s no lut available.\n", __func__ );
		ret = -ETIMEDOUT;
		return ret;
	}

	lut = epdc_get_next_lut();
	mxc_epdc_fb_read_temperature(fb_data);
	epdc_set_temp(fb_data->temp_index);
	// we should exchange top and left depending on rotate !
	epdc_set_update_coord( updata->update_region.left, updata->update_region.top );
	epdc_set_update_dimensions( updata->update_region.width,
				    updata->update_region.height );
	epdc_submit_update( lut, updata->waveform_mode,
			    updata->update_mode, true, val );

	if ( !wait_update )	// no wait completion of update
	{
		return ret;
	}
 
	// wait display
	retry = 60;
	while( epdc_is_lut_active(lut) ||
		epdc_is_working_buffer_busy() )
	{
		msleep(50);
		retry--;
		if ( retry == 0 )
		{
			printk( KERN_ERR "%s  black complete timeout.\n", __func__ );
			break;
		}
	}


	return ret;
}

/* 2011/05/12 FY11 : Supported boot progress. */
static void epdc_progress_work_func(struct work_struct *work)
{
	int ret, i=0;
	struct mxcfb_update_data updata;
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data,
			epdc_progress_work.work);

	mutex_lock(&(fb_data->info.lock));

	printk( "%s start progress.\n", __func__ );

	updata.update_region.left = 521;
	updata.update_region.top = 408;
	updata.update_region.width = 6;
	updata.update_region.height = 6;
	updata.waveform_mode = fb_data->wv_modes.mode_du;
	updata.update_mode = UPDATE_MODE_PARTIAL;

	// powerup
	ret = epdc_powerup(fb_data);
	if ( ret < 0 )
	{
		printk( KERN_ERR "%s Fails to powerup.%d\n", __func__, ret );
	}
	else
	{
		for ( i = 0; i < 35; i++ )
		{
			updata.update_region.top -= updata.update_region.height;
			if ( atomic_read( &(fb_data->info.lock.count) ) < 0 )
			{
				epdc_draw_rect( fb_data, &updata, 0x00, false );
				msleep(50);
			}
			else
			{
				epdc_draw_rect( fb_data, &updata, 0x00, true );
				msleep(10);
			}
		}
		updata.update_region.top -= updata.update_region.height;
		epdc_draw_rect( fb_data, &updata, 0x00, true );

		// powerdown
/* 2011/06/06 FY11 : Fixed the failure of power off. */
		fb_data->powering_down = true;
		schedule_delayed_work(&fb_data->epdc_done_work,
					msecs_to_jiffies(fb_data->pwrdown_delay));
	}

	printk( "%s end progress.\n", __func__ );

	mutex_unlock(&(fb_data->info.lock));
}


static void epdc_init_sequence(struct mxc_epdc_fb_data *fb_data)
{

	/* Initialize EPDC, passing pointer to EPDC registers */
	epdc_init_settings(fb_data);
	__raw_writel(fb_data->waveform_buffer_phys, EPDC_WVADDR);
	__raw_writel(fb_data->working_buffer_phys, EPDC_WB_ADDR);
}

static int mxc_epdc_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	u32 len;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset < info->fix.smem_len) {
		/* mapping framebuffer memory */
		len = info->fix.smem_len - offset;
		vma->vm_pgoff = (info->fix.smem_start + offset) >> PAGE_SHIFT;
	} else
		return -EINVAL;

	len = PAGE_ALIGN(len);
	if (vma->vm_end - vma->vm_start > len)
		return -EINVAL;

	/* make buffers bufferable */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	vma->vm_flags |= VM_IO | VM_RESERVED;

	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		dev_dbg(info->device, "mmap remap_pfn_range failed\n");
		return -ENOBUFS;
	}

	return 0;
}

static int mxc_epdc_fb_setcolreg(u_int regno, u_int red, u_int green,
				 u_int blue, u_int transp, struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;
	}

#define CNVT_TOHW(val, width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {

		if (regno >= 16)
			return 1;

		((u32 *) (info->pseudo_palette))[regno] =
		    (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
	}
	return 0;
}

static void adjust_coordinates(struct mxc_epdc_fb_data *fb_data,
	struct mxcfb_rect *update_region, struct mxcfb_rect *adj_update_region)
{
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 rotation = fb_data->epdc_fb_var.rotate;
	u32 temp;

	/* If adj_update_region == NULL, pass result back in update_region */
	/* If adj_update_region == valid, use it to pass back result */
	if (adj_update_region)
		switch (rotation) {
		case FB_ROTATE_UR:
			adj_update_region->top = update_region->top;
			adj_update_region->left = update_region->left;
			adj_update_region->width = update_region->width;
			adj_update_region->height = update_region->height;
			break;
		case FB_ROTATE_CW:
			adj_update_region->top = update_region->left;
			adj_update_region->left = screeninfo->yres -
				(update_region->top + update_region->height);
			adj_update_region->width = update_region->height;
			adj_update_region->height = update_region->width;
			break;
		case FB_ROTATE_UD:
			adj_update_region->width = update_region->width;
			adj_update_region->height = update_region->height;
			adj_update_region->top = screeninfo->yres -
				(update_region->top + update_region->height);
			adj_update_region->left = screeninfo->xres -
				(update_region->left + update_region->width);
			break;
		case FB_ROTATE_CCW:
			adj_update_region->left = update_region->top;
			adj_update_region->top = screeninfo->xres -
				(update_region->left + update_region->width);
			adj_update_region->width = update_region->height;
			adj_update_region->height = update_region->width;
			break;
		}
	else
		switch (rotation) {
		case FB_ROTATE_UR:
			/* No adjustment needed */
			break;
		case FB_ROTATE_CW:
			temp = update_region->top;
			update_region->top = update_region->left;
			update_region->left = screeninfo->yres -
				(temp + update_region->height);
			temp = update_region->width;
			update_region->width = update_region->height;
			update_region->height = temp;
			break;
		case FB_ROTATE_UD:
			update_region->top = screeninfo->yres -
				(update_region->top + update_region->height);
			update_region->left = screeninfo->xres -
				(update_region->left + update_region->width);
			break;
		case FB_ROTATE_CCW:
			temp = update_region->left;
			update_region->left = update_region->top;
			update_region->top = screeninfo->xres -
				(temp + update_region->width);
			temp = update_region->width;
			update_region->width = update_region->height;
			update_region->height = temp;
			break;
		}
}

/*
 * Set fixed framebuffer parameters based on variable settings.
 *
 * @param       info     framebuffer information pointer
 */
static int mxc_epdc_fb_set_fix(struct fb_info *info)
{
	struct fb_fix_screeninfo *fix = &info->fix;
	struct fb_var_screeninfo *var = &info->var;

	fix->line_length = var->xres_virtual * var->bits_per_pixel / 8;

	fix->type = FB_TYPE_PACKED_PIXELS;
	fix->accel = FB_ACCEL_NONE;
	if (var->grayscale)
		fix->visual = FB_VISUAL_STATIC_PSEUDOCOLOR;
	else
		fix->visual = FB_VISUAL_TRUECOLOR;
	fix->xpanstep = 1;
	fix->ypanstep = 1;

	return 0;
}

/*
 * This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the
 * change in par. For this driver it doesn't do much.
 *
 */
static int mxc_epdc_fb_set_par(struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	struct pxp_config_data *pxp_conf = &fb_data->pxp_conf;
	struct pxp_proc_data *proc_data = &pxp_conf->proc_data;
	struct fb_var_screeninfo *screeninfo = &fb_data->info.var;
	struct mxc_epdc_fb_mode *epdc_modes = fb_data->pdata->epdc_mode;
	int i;
	int ret;
	unsigned long flags;
	__u32 xoffset_old, yoffset_old;

	/*
	 * Can't change the FB parameters until current updates have completed.
	 * This function returns when all active updates are done.
	 */
	mxc_epdc_fb_flush_updates(fb_data);

	spin_lock_irqsave(&fb_data->queue_lock, flags);
	/*
	 * Set all screeninfo except for xoffset/yoffset
	 * Subsequent call to pan_display will handle those.
	 */
	xoffset_old = fb_data->epdc_fb_var.xoffset;
	yoffset_old = fb_data->epdc_fb_var.yoffset;
	fb_data->epdc_fb_var = *screeninfo;
	fb_data->epdc_fb_var.xoffset = xoffset_old;
	fb_data->epdc_fb_var.yoffset = yoffset_old;
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	mutex_lock(&fb_data->pxp_mutex);

	/*
	 * Update PxP config data (used to process FB regions for updates)
	 * based on FB info and processing tasks required
	 */

	/* Initialize non-channel-specific PxP parameters */
	proc_data->drect.left = proc_data->srect.left = 0;
	proc_data->drect.top = proc_data->srect.top = 0;
	proc_data->drect.width = proc_data->srect.width = screeninfo->xres;
	proc_data->drect.height = proc_data->srect.height = screeninfo->yres;
	proc_data->scaling = 0;
	proc_data->hflip = 0;
	proc_data->vflip = 0;
	proc_data->rotate = screeninfo->rotate;
	proc_data->bgcolor = 0;
	proc_data->overlay_state = 0;
	proc_data->lut_transform = PXP_LUT_NONE;

	/*
	 * configure S0 channel parameters
	 * Parameters should match FB format/width/height
	 */
	if (screeninfo->grayscale)
		pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_GREY;
	else {
		switch (screeninfo->bits_per_pixel) {
		case 16:
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB565;
			break;
		case 24:
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB24;
			break;
		case 32:
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB32;
			break;
		default:
			pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB565;
			break;
		}
	}
	pxp_conf->s0_param.width = screeninfo->xres_virtual;
	pxp_conf->s0_param.height = screeninfo->yres;
	pxp_conf->s0_param.color_key = -1;
	pxp_conf->s0_param.color_key_enable = false;

	/*
	 * Initialize Output channel parameters
	 * Output is Y-only greyscale
	 * Output width/height will vary based on update region size
	 */
	pxp_conf->out_param.width = screeninfo->xres;
	pxp_conf->out_param.height = screeninfo->yres;
	pxp_conf->out_param.pixel_fmt = PXP_PIX_FMT_GREY;

	mutex_unlock(&fb_data->pxp_mutex);

	/*
	 * If HW not yet initialized, check to see if we are being sent
	 * an initialization request.
	 */
	if (!fb_data->hw_ready) {
		struct fb_videomode mode;
		bool found_match = false;
		u32 xres_temp;

		fb_var_to_videomode(&mode, screeninfo);

		/* When comparing requested fb mode,
		   we need to use unrotated dimensions */
		if ((screeninfo->rotate == FB_ROTATE_CW) ||
			(screeninfo->rotate == FB_ROTATE_CCW)) {
			xres_temp = mode.xres;
			mode.xres = mode.yres;
			mode.yres = xres_temp;
		}

		/* Match videomode against epdc modes */
		for (i = 0; i < fb_data->pdata->num_modes; i++) {
			if (!fb_mode_is_equal(epdc_modes[i].vmode, &mode))
				continue;
			fb_data->cur_mode = &epdc_modes[i];
			found_match = true;
			break;
		}

		if (!found_match) {
			dev_err(fb_data->dev,
				"Failed to match requested video mode\n");
			return EINVAL;
		}

		/* Found a match - Grab timing params */
		screeninfo->left_margin = mode.left_margin;
		screeninfo->right_margin = mode.right_margin;
		screeninfo->upper_margin = mode.upper_margin;
		screeninfo->lower_margin = mode.lower_margin;
		screeninfo->hsync_len = mode.hsync_len;
		screeninfo->vsync_len = mode.vsync_len;

		/* Initialize EPDC settings and init panel */
		ret =
		    mxc_epdc_fb_init_hw((struct fb_info *)fb_data);
		if (ret) {
			dev_err(fb_data->dev,
				"Failed to load panel waveform data\n");
			return ret;
		}
	}

	/*
	 * EOF sync delay (in us) should be equal to the vscan holdoff time
	 * VSCAN_HOLDOFF time = (VSCAN_HOLDOFF value + 1) * Vertical lines
	 * Add 25us for additional margin
	 */
	fb_data->eof_sync_period = (fb_data->cur_mode->vscan_holdoff + 1) *
		1000000/(fb_data->cur_mode->vmode->refresh *
		(fb_data->cur_mode->vmode->upper_margin +
		fb_data->cur_mode->vmode->yres +
		fb_data->cur_mode->vmode->lower_margin +
		fb_data->cur_mode->vmode->vsync_len)) + 25;

	mxc_epdc_fb_set_fix(info);

	return 0;
}

static int mxc_epdc_fb_check_var(struct fb_var_screeninfo *var,
				 struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;

	if (!var->xres)
		var->xres = 1;
	if (!var->yres)
		var->yres = 1;

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	if ((var->bits_per_pixel != 32) && (var->bits_per_pixel != 24) &&
	    (var->bits_per_pixel != 16) && (var->bits_per_pixel != 8))
		var->bits_per_pixel = default_bpp;

	switch (var->bits_per_pixel) {
	case 8:
		if (var->grayscale != 0) {
			/*
			 * For 8-bit grayscale, R, G, and B offset are equal.
			 *
			 */
			var->red.length = 8;
			var->red.offset = 0;
			var->red.msb_right = 0;

			var->green.length = 8;
			var->green.offset = 0;
			var->green.msb_right = 0;

			var->blue.length = 8;
			var->blue.offset = 0;
			var->blue.msb_right = 0;

			var->transp.length = 0;
			var->transp.offset = 0;
			var->transp.msb_right = 0;
		} else {
			var->red.length = 3;
			var->red.offset = 5;
			var->red.msb_right = 0;

			var->green.length = 3;
			var->green.offset = 2;
			var->green.msb_right = 0;

			var->blue.length = 2;
			var->blue.offset = 0;
			var->blue.msb_right = 0;

			var->transp.length = 0;
			var->transp.offset = 0;
			var->transp.msb_right = 0;
		}
		break;
	case 16:
		var->red.length = 5;
		var->red.offset = 11;
		var->red.msb_right = 0;

		var->green.length = 6;
		var->green.offset = 5;
		var->green.msb_right = 0;

		var->blue.length = 5;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 0;
		var->transp.offset = 0;
		var->transp.msb_right = 0;
		break;
	case 24:
		var->red.length = 8;
		var->red.offset = 16;
		var->red.msb_right = 0;

		var->green.length = 8;
		var->green.offset = 8;
		var->green.msb_right = 0;

		var->blue.length = 8;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 0;
		var->transp.offset = 0;
		var->transp.msb_right = 0;
		break;
	case 32:
		var->red.length = 8;
		var->red.offset = 16;
		var->red.msb_right = 0;

		var->green.length = 8;
		var->green.offset = 8;
		var->green.msb_right = 0;

		var->blue.length = 8;
		var->blue.offset = 0;
		var->blue.msb_right = 0;

		var->transp.length = 8;
		var->transp.offset = 24;
		var->transp.msb_right = 0;
		break;
	}

	switch (var->rotate) {
	case FB_ROTATE_UR:
	case FB_ROTATE_UD:
		var->xres = fb_data->native_width;
		var->yres = fb_data->native_height;
		break;
	case FB_ROTATE_CW:
	case FB_ROTATE_CCW:
		var->xres = fb_data->native_height;
		var->yres = fb_data->native_width;
		break;
	default:
		/* Invalid rotation value */
		var->rotate = 0;
		dev_dbg(fb_data->dev, "Invalid rotation request\n");
		return -EINVAL;
	}

	var->xres_virtual = ALIGN(var->xres, 32);
	var->yres_virtual = ALIGN(var->yres, 128) * fb_data->num_screens;

	var->height = -1;
	var->width = -1;

	return 0;
}

void mxc_epdc_fb_set_waveform_modes(struct mxcfb_waveform_modes *modes,
	struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	memcpy(&fb_data->wv_modes, modes, sizeof(modes));
}
EXPORT_SYMBOL(mxc_epdc_fb_set_waveform_modes);

static int mxc_epdc_fb_get_temp_index(struct mxc_epdc_fb_data *fb_data, int temp)
{
	int i;
	int index = -1;

	if (fb_data->trt_entries == 0) {
		dev_err(fb_data->dev,
			"No TRT exists...using default temp index\n");
		return DEFAULT_TEMP_INDEX;
	}

	/* Search temperature ranges for a match */
	for (i = 0; i < fb_data->trt_entries - 1; i++) {
		if ((temp >= fb_data->temp_range_bounds[i])
			&& (temp < fb_data->temp_range_bounds[i+1])) {
			index = i;
			break;
		}
	}

	if (index < 0) {
/* 2011/2/24 FY11 : Modified to use lower or upper limit index when temp is out of range. */
		if ( temp <= fb_data->temp_range_bounds[0] )
		{
		//	printk( KERN_INFO "temperature is lower than limit.\n" );
			index = 0;
		}
		else
		{
		//	printk( KERN_INFO "temperature is higher than limit.\n" );
			index = (fb_data->trt_entries) - 1;
		}
	}

	dev_dbg(fb_data->dev, "Using temperature index %d\n", index);

	return index;
}

/* 2011/2/14 FY11 : Supported auto temperature reading. */
int mxc_epdc_fb_read_temperature(struct mxc_epdc_fb_data *fb_data)
{
	int temperature;
	int savedPowerState, ret=0;

	if ((savedPowerState = fb_data->power_state) == POWER_STATE_OFF)
		regulator_enable(fb_data->display_regulator);
	temperature = regulator_get_voltage(fb_data->temp_regulator);
	if (savedPowerState == POWER_STATE_OFF)
		regulator_disable(fb_data->display_regulator);

	if (temperature < 0 )	// err
	{
		ret = temperature;
		printk( KERN_ERR "Fail to read temperature! %d\n", ret );
	}
	else
	{
		if ( temperature >= MINUS_TEMPERATURE )
		{
			temperature -= MINUS_TEMP_ADJUST_VAL;
		}
		fb_data->temp_index = mxc_epdc_fb_get_temp_index(fb_data, temperature);
		epdc_debug_printk( " temperature is %d (index=%d).\n", temperature, fb_data->temp_index );
	}

	return ret;
}

int mxc_epdc_fb_set_temperature(int temperature, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;
	unsigned long flags;

	/* Store temp index. Used later when configuring updates. */
	spin_lock_irqsave(&fb_data->queue_lock, flags);
	fb_data->temp_index = mxc_epdc_fb_get_temp_index(fb_data, temperature);
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	return 0;
}


static int mxc_epdc_fb_set_border_mode(int border_mode, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	int savedPowerState;
	int ret = 0;

	if ((savedPowerState = fb_data->power_state) == POWER_STATE_OFF)
	{
/* 2011/03/09 FY11 : Fixed the bug that CPU hangs caused by accessing to EPDC without clock. */
		ret = epdc_powerup(fb_data);
		if ( ret < 0 )
		{
			printk( KERN_ERR "%s Fails to powerup. %d\n", __func__, ret );
			return ret;
		}
	}

	epdc_set_border_gpio(border_mode);

	if (savedPowerState == POWER_STATE_OFF)
		epdc_powerdown(fb_data);
        
    return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_temperature);

int mxc_epdc_fb_set_auto_update(u32 auto_mode, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	dev_dbg(fb_data->dev, "Setting auto update mode to %d\n", auto_mode);

	if ((auto_mode == AUTO_UPDATE_MODE_AUTOMATIC_MODE)
		|| (auto_mode == AUTO_UPDATE_MODE_REGION_MODE))
		fb_data->auto_mode = auto_mode;
	else {
		dev_err(fb_data->dev, "Invalid auto update mode parameter.\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_auto_update);

int mxc_epdc_fb_set_upd_scheme(u32 upd_scheme, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	dev_dbg(fb_data->dev, "Setting optimization level to %d\n", upd_scheme);

	/*
	 * Can't change the scheme until current updates have completed.
	 * This function returns when all active updates are done.
	 */
	mxc_epdc_fb_flush_updates(fb_data);

	if ((upd_scheme == UPDATE_SCHEME_SNAPSHOT)
		|| (upd_scheme == UPDATE_SCHEME_QUEUE)
		|| (upd_scheme == UPDATE_SCHEME_QUEUE_AND_MERGE))
		fb_data->upd_scheme = upd_scheme;
	else {
		dev_err(fb_data->dev, "Invalid update scheme specified.\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_upd_scheme);

static void copy_before_process(struct mxc_epdc_fb_data *fb_data,
	struct update_data_list *upd_data_list)
{
	int i;
	unsigned char *temp_buf_ptr = fb_data->virt_addr_copybuf;
	unsigned char *src_ptr;
	struct mxcfb_rect *src_upd_region;
	int temp_buf_stride;
	int src_stride;
	int bpp = fb_data->epdc_fb_var.bits_per_pixel;
	int left_offs, right_offs;
	int x_trailing_bytes, y_trailing_bytes;

	/* Set source buf pointer based on input source, panning, etc. */
	if (upd_data_list->upd_data.flags & EPDC_FLAG_USE_ALT_BUFFER) {
		src_upd_region = &upd_data_list->upd_data.alt_buffer_data.alt_update_region;
		src_stride =
			upd_data_list->upd_data.alt_buffer_data.width * bpp/8;
		src_ptr = upd_data_list->upd_data.alt_buffer_data.virt_addr
			+ src_upd_region->top * src_stride;
	} else {
		src_upd_region = &upd_data_list->upd_data.update_region;
		src_stride = fb_data->epdc_fb_var.xres_virtual * bpp/8;
		src_ptr = fb_data->info.screen_base + fb_data->fb_offset
			+ src_upd_region->top * src_stride;
	}

	temp_buf_stride = ALIGN(src_upd_region->width, 8) * bpp/8;
	left_offs = src_upd_region->left * bpp/8;
	right_offs = src_upd_region->width * bpp/8;
	x_trailing_bytes = (ALIGN(src_upd_region->width, 8)
		- src_upd_region->width) * bpp/8;

	for (i = 0; i < src_upd_region->height; i++) {
		/* Copy the full line */
		memcpy(temp_buf_ptr, src_ptr + left_offs,
			src_upd_region->width * bpp/8);

		/* Clear any unwanted pixels at the end of each line */
		if (src_upd_region->width & 0x7) {
			memset(temp_buf_ptr + right_offs, 0x0,
				x_trailing_bytes);
		}

		temp_buf_ptr += temp_buf_stride;
		src_ptr += src_stride;
	}

	/* Clear any unwanted pixels at the bottom of the end of each line */
	if (src_upd_region->height & 0x7) {
		y_trailing_bytes = (ALIGN(src_upd_region->height, 8)
			- src_upd_region->height) *
			ALIGN(src_upd_region->width, 8) * bpp/8;
		memset(temp_buf_ptr, 0x0, y_trailing_bytes);
	}
}

static int epdc_process_update(struct update_data_list *upd_data_list,
				   struct mxc_epdc_fb_data *fb_data)
{
	struct mxcfb_rect *src_upd_region; /* Region of src buffer for update */
	struct mxcfb_rect pxp_upd_region;
	u32 src_width, src_height;
	u32 offset_from_4, bytes_per_pixel;
	u32 post_rotation_xcoord, post_rotation_ycoord, width_pxp_blocks;
	u32 pxp_input_offs, pxp_output_offs, pxp_output_shift;
	u32 hist_stat = 0;
	int width_unaligned, height_unaligned;
	bool input_unaligned = false;
	bool line_overflow = false;
	int pix_per_line_added;
	bool use_temp_buf = false;
	struct mxcfb_rect temp_buf_upd_region;

	int ret, ret_powerup = 0;

	/*
	 * Gotta do a whole bunch of buffer ptr manipulation to
	 * work around HW restrictions for PxP & EPDC
	 */

	/*
	 * Are we using FB or an alternate (overlay)
	 * buffer for source of update?
	 */
	if (upd_data_list->upd_data.flags & EPDC_FLAG_USE_ALT_BUFFER) {
		src_width = upd_data_list->upd_data.alt_buffer_data.width;
		src_height = upd_data_list->upd_data.alt_buffer_data.height;
		src_upd_region = &upd_data_list->upd_data.alt_buffer_data.alt_update_region;
	} else {
		src_width = fb_data->epdc_fb_var.xres_virtual;
		src_height = fb_data->epdc_fb_var.yres;
		src_upd_region = &upd_data_list->upd_data.update_region;
	}

	bytes_per_pixel = fb_data->epdc_fb_var.bits_per_pixel/8;

	/*
	 * SW workaround for PxP limitation
	 *
	 * There are 3 cases where we cannot process the update data
	 * directly from the input buffer:
	 *
	 * 1) PxP must process 8x8 pixel blocks, and all pixels in each block
	 * are considered for auto-waveform mode selection. If the
	 * update region is not 8x8 aligned, additional unwanted pixels
	 * will be considered in auto-waveform mode selection.
	 *
	 * 2) PxP input must be 32-bit aligned, so any update
	 * address not 32-bit aligned must be shifted to meet the
	 * 32-bit alignment.  The PxP will thus end up processing pixels
	 * outside of the update region to satisfy this alignment restriction,
	 * which can affect auto-waveform mode selection.
	 *
	 * 3) If input fails 32-bit alignment, and the resulting expansion
	 * of the processed region would add at least 8 pixels more per
	 * line than the original update line width, the EPDC would
	 * cause screen artifacts by incorrectly handling the 8+ pixels
	 * at the end of each line.
	 *
	 * Workaround is to copy from source buffer into a temporary
	 * buffer, which we pad with zeros to match the 8x8 alignment
	 * requirement. This temp buffer becomes the input to the PxP.
	 */
	width_unaligned = src_upd_region->width & 0x7;
	height_unaligned = src_upd_region->height & 0x7;

	offset_from_4 = src_upd_region->left & 0x3;
	input_unaligned = ((offset_from_4 * bytes_per_pixel % 4) != 0) ?
				true : false;

	pix_per_line_added = (offset_from_4 * bytes_per_pixel % 4) / bytes_per_pixel;
	if ((((fb_data->epdc_fb_var.rotate == FB_ROTATE_UR) ||
		fb_data->epdc_fb_var.rotate == FB_ROTATE_UD)) &&
		(ALIGN(src_upd_region->width, 8) <
			ALIGN(src_upd_region->width + pix_per_line_added, 8)))
		line_overflow = true;

	/* Grab pxp_mutex here so that we protect access
	 * to copybuf in addition to the PxP structures */
	mutex_lock(&fb_data->pxp_mutex);

	if (((width_unaligned || height_unaligned || input_unaligned) &&
		(upd_data_list->upd_data.waveform_mode == WAVEFORM_MODE_AUTO))
		|| line_overflow) {

		dev_dbg(fb_data->dev, "Copying update before processing.\n");

		/* Update to reflect what the new source buffer will be */
		src_width = ALIGN(src_upd_region->width, 8);
		src_height = ALIGN(src_upd_region->height, 8);

		copy_before_process(fb_data, upd_data_list);

		/*
		 * src_upd_region should now describe
		 * the new update buffer attributes.
		 */
		temp_buf_upd_region.left = 0;
		temp_buf_upd_region.top = 0;
		temp_buf_upd_region.width = src_upd_region->width;
		temp_buf_upd_region.height = src_upd_region->height;
		src_upd_region = &temp_buf_upd_region;

		use_temp_buf = true;
	}

	/*
	 * Compute buffer offset to account for
	 * PxP limitation (input must be 32-bit aligned)
	 */
	offset_from_4 = src_upd_region->left & 0x3;
	input_unaligned = ((offset_from_4 * bytes_per_pixel % 4) != 0) ?
				true : false;
	if (input_unaligned) {
		/* Leave a gap between PxP input addr and update region pixels */
		pxp_input_offs =
			(src_upd_region->top * src_width + src_upd_region->left)
			* bytes_per_pixel & 0xFFFFFFFC;
		/* Update region should change to reflect relative position to input ptr */
		pxp_upd_region.top = 0;
		pxp_upd_region.left = (offset_from_4 * bytes_per_pixel % 4) / bytes_per_pixel;
	} else {
		pxp_input_offs =
			(src_upd_region->top * src_width + src_upd_region->left)
			* bytes_per_pixel;
		/* Update region should change to reflect relative position to input ptr */
		pxp_upd_region.top = 0;
		pxp_upd_region.left = 0;
	}

	/* Update region dimensions to meet 8x8 pixel requirement */
	pxp_upd_region.width =
		ALIGN(src_upd_region->width + pxp_upd_region.left, 8);
	pxp_upd_region.height = ALIGN(src_upd_region->height, 8);

	switch (fb_data->epdc_fb_var.rotate) {
	case FB_ROTATE_UR:
	default:
		post_rotation_xcoord = pxp_upd_region.left;
		post_rotation_ycoord = pxp_upd_region.top;
		width_pxp_blocks = pxp_upd_region.width;
		break;
	case FB_ROTATE_CW:
		width_pxp_blocks = pxp_upd_region.height;
		post_rotation_xcoord = width_pxp_blocks - src_upd_region->height;
		post_rotation_ycoord = pxp_upd_region.left;
		break;
	case FB_ROTATE_UD:
		width_pxp_blocks = pxp_upd_region.width;
		post_rotation_xcoord = width_pxp_blocks - src_upd_region->width - pxp_upd_region.left;
		post_rotation_ycoord = pxp_upd_region.height - src_upd_region->height - pxp_upd_region.top;
		break;
	case FB_ROTATE_CCW:
		width_pxp_blocks = pxp_upd_region.height;
		post_rotation_xcoord = pxp_upd_region.top;
		post_rotation_ycoord = pxp_upd_region.width - src_upd_region->width - pxp_upd_region.left;
		break;
	}

	/* Update region start coord to force PxP to process full 8x8 regions */
	pxp_upd_region.top &= ~0x7;
	pxp_upd_region.left &= ~0x7;

	pxp_output_shift = ALIGN(post_rotation_xcoord, 8)
		- post_rotation_xcoord;

	pxp_output_offs = post_rotation_ycoord * width_pxp_blocks
		+ pxp_output_shift;

	upd_data_list->epdc_offs = ALIGN(pxp_output_offs, 8);

	/* Source address either comes from alternate buffer
	   provided in update data, or from the framebuffer. */
	if (use_temp_buf)
		sg_dma_address(&fb_data->sg[0]) =
 			fb_data->phys_addr_copybuf;
	else if (upd_data_list->upd_data.flags & EPDC_FLAG_USE_ALT_BUFFER)
		sg_dma_address(&fb_data->sg[0]) =
			upd_data_list->upd_data.alt_buffer_data.phys_addr
				+ pxp_input_offs;
	else {
/* 2011/03/15 FY11 : Supported standby screen. */
		if ( upd_data_list->use_standbyscreen )
		{
			sg_dma_address( &fb_data->sg[0] ) =
				fb_data->standbyscreen_buff;
			sg_set_page(&fb_data->sg[0],
				virt_to_page(fb_data->standbyscreen_buff_virt),
				fb_data->standbyscreen_buff_size,
				offset_in_page(fb_data->standbyscreen_buff_virt));
		}
		else
		{
			sg_dma_address(&fb_data->sg[0]) =
				fb_data->info.fix.smem_start + fb_data->fb_offset
				+ pxp_input_offs;
			sg_set_page(&fb_data->sg[0],
				virt_to_page(fb_data->info.screen_base),
				fb_data->info.fix.smem_len,
				offset_in_page(fb_data->info.screen_base));
		}
	}

	/* Update sg[1] to point to output of PxP proc task */
	sg_dma_address(&fb_data->sg[1]) = upd_data_list->phys_addr
						+ pxp_output_shift;
	sg_set_page(&fb_data->sg[1], virt_to_page(upd_data_list->virt_addr),
		    fb_data->max_pix_size,
		    offset_in_page(upd_data_list->virt_addr));

	/*
	 * Set PxP LUT transform type based on update flags.
	 */
	fb_data->pxp_conf.proc_data.lut_transform = 0;
	if (upd_data_list->upd_data.flags & EPDC_FLAG_ENABLE_INVERSION)
		fb_data->pxp_conf.proc_data.lut_transform |= PXP_LUT_INVERT;
	if (upd_data_list->upd_data.flags & EPDC_FLAG_FORCE_MONOCHROME)
		fb_data->pxp_conf.proc_data.lut_transform |=
			PXP_LUT_BLACK_WHITE;

	/*
	 * Toggle inversion processing if 8-bit
	 * inverted is the current pixel format.
	 */
	if (fb_data->epdc_fb_var.grayscale == GRAYSCALE_8BIT_INVERTED)
		fb_data->pxp_conf.proc_data.lut_transform ^= PXP_LUT_INVERT;

	/* This is a blocking call, so upon return PxP tx should be done */
	ret = pxp_process_update(fb_data, src_width, src_height,
		&pxp_upd_region);
	if (ret) {
		dev_err(fb_data->dev, "Unable to submit PxP update task.\n");
		mutex_unlock(&fb_data->pxp_mutex);
		return ret;
	}

	/* If needed, enable EPDC HW while ePxP is processing */
	if ((fb_data->power_state == POWER_STATE_OFF)
		|| fb_data->powering_down) {
/* 2011/03/09 FY11 : Fixed the bug that CPU hangs caused by accessing to EPDC without clock. */
		ret_powerup = epdc_powerup(fb_data);
	}

	/* This is a blocking call, so upon return PxP tx should be done */
	ret = pxp_complete_update(fb_data, &hist_stat);
	if (ret) {
		dev_err(fb_data->dev, "Unable to complete PxP update task.\n");
/* 2011/03/09 FY11 : Fixed the bug that CPU hangs caused by accessing to EPDC without clock. */
		if ( ret_powerup == 0 )
		{
/* 2011/06/09 FY11 : Fixed the failure of power off. */
			fb_data->powering_down = true;
			schedule_delayed_work(&fb_data->epdc_done_work,
						msecs_to_jiffies(fb_data->pwrdown_delay));
		}
		mutex_unlock(&fb_data->pxp_mutex);
		return ret;
	}

	mutex_unlock(&fb_data->pxp_mutex);

/* 2011/03/09 FY11 : Fixed the bug that CPU hangs caused by accessing to EPDC without clock. */
	if( ret_powerup < 0 )
	{
		printk( KERN_ERR "%s fails to powerup.%d\n", __func__, ret_powerup );
		return ret_powerup;
	}
	else
	{
/* 2011/07/05 FY11 : Added workaround for EPD PMIC heat up. */
		if ( ! regulator_is_enabled( fb_data->epd_pwr0_regulator ) )
		{
			dev_err(fb_data->dev, "EPD PMIC is in abnormal condition.\n");
			mutex_lock(&fb_data->power_mutex);
			/* Never turn on epd pmic and disable VSYS_EPD later. */
			epdc_powerup_err_cnt = EPDC_PROHIBITION_THRESH;
			regulator_disable(fb_data->vsys_regulator);
			mutex_unlock(&fb_data->power_mutex);
			return -EIO;
		}
	}

	/* Update waveform mode from PxP histogram results */
	if (upd_data_list->upd_data.waveform_mode == WAVEFORM_MODE_AUTO) {
		if (hist_stat & 0x1)
			upd_data_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_du;
		else if (hist_stat & 0x2)
			upd_data_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc4;
		else if (hist_stat & 0x4)
			upd_data_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc8;
		else if (hist_stat & 0x8)
			upd_data_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc16;
		else
			upd_data_list->upd_data.waveform_mode =
				fb_data->wv_modes.mode_gc32;

		dev_dbg(fb_data->dev, "hist_stat = 0x%x, new waveform = 0x%x\n",
			hist_stat, upd_data_list->upd_data.waveform_mode);
	}

	return 0;

}

static int epdc_submit_merge(struct update_data_list *upd_data_list,
				struct update_data_list *update_to_merge)
{
	struct mxcfb_update_data *a, *b;
	struct mxcfb_rect *arect, *brect;
	struct mxcfb_rect combine;
	bool use_flags = false;

	a = &upd_data_list->upd_data;
	b = &update_to_merge->upd_data;
	arect = &upd_data_list->upd_data.update_region;
	brect = &update_to_merge->upd_data.update_region;

	/*
	 * Updates with different flags must be executed sequentially.
	 * Halt the merge process to ensure this.
	 */
	if (a->flags != b->flags) {
		/*
		 * Special exception: if update regions are identical,
		 * we may be able to merge them.
		 */
		if ((arect->left != brect->left) ||
			(arect->top != brect->top) ||
			(arect->top == brect->width) ||
			(arect->top == brect->height))
			return MERGE_BLOCK;

		use_flags = true;
	}

	if ((a->waveform_mode != b->waveform_mode
		&& a->waveform_mode != WAVEFORM_MODE_AUTO) ||
		a->update_mode != b->update_mode ||
		arect->left > (brect->left + brect->width) ||
		brect->left > (arect->left + arect->width) ||
		arect->top > (brect->top + brect->height) ||
		brect->top > (arect->top + arect->height) ||
		(b->update_marker != 0 && a->update_marker != 0) ||
/* 2011/03/05 FY11 : Supported A2 mode limitations. */
/* 2011/03/15 FY11 : Supported standby screen. */
		(upd_data_list->use_white_buf || update_to_merge->use_white_buf) || 
		(upd_data_list->use_standbyscreen || update_to_merge->use_standbyscreen) )
		return MERGE_FAIL;

	combine.left = arect->left < brect->left ? arect->left : brect->left;
	combine.top = arect->top < brect->top ? arect->top : brect->top;
	combine.width = (arect->left + arect->width) >
			(brect->left + brect->width) ?
			(arect->left + arect->width - combine.left) :
			(brect->left + brect->width - combine.left);
	combine.height = (arect->top + arect->height) >
			(brect->top + brect->height) ?
			(arect->top + arect->height - combine.top) :
			(brect->top + brect->height - combine.top);

	*arect = combine;

	/* Use flags of the later update */
	if (use_flags)
		a->flags = b->flags;

	/* Preserve marker value for merged update */
	if (b->update_marker != 0) {
		a->update_marker = b->update_marker;
		upd_data_list->upd_marker_data =
			update_to_merge->upd_marker_data;
	}

	/* Merged update should take on the earliest order */
	upd_data_list->update_order =
		(upd_data_list->update_order > update_to_merge->update_order) ?
		upd_data_list->update_order : update_to_merge->update_order;

	return MERGE_OK;
}



/* 2011/04/19 FY11 : Supported wake lock. */
static void epdc_wake_lock_for_update_buffer(struct mxc_epdc_fb_data *fb_data )
{
	if ( fb_data->counter_for_wlupdate == 0 )
	{
		wake_lock( &(fb_data->wake_lock_update) );
	}
	fb_data->counter_for_wlupdate++;
	
	epdc_debug_printk( "counter for update buffer wake lock = %d\n", fb_data->counter_for_wlupdate ); 
}

static void epdc_wake_unlock_for_update_buffer(struct mxc_epdc_fb_data *fb_data )
{
	fb_data->counter_for_wlupdate--;
	if ( fb_data->counter_for_wlupdate == 0 )
	{
		wake_unlock( &(fb_data->wake_lock_update) );
	}
	else if( fb_data->counter_for_wlupdate < 0 )
	{
		printk( KERN_ERR "%s invalid wake lock counter %d!!!\n", __func__, fb_data->counter_for_wlupdate );
	}
	epdc_debug_printk( "counter for update buffer wake lock = %d\n", fb_data->counter_for_wlupdate ); 
}


static void epdc_submit_work_func(struct work_struct *work)
{
	int temp_index;
	struct update_data_list *next_update;
	struct update_data_list *temp;
	unsigned long flags;
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data, epdc_submit_work);
	struct update_data_list *upd_data_list = NULL;
	struct mxcfb_rect adj_update_region;
	bool end_merge = false;
	int ret;

	/* Protect access to buffer queues and to update HW */
	spin_lock_irqsave(&fb_data->queue_lock, flags);

	/*
	 * Are any of our collision updates able to go now?
	 * Go through all updates in the collision list and check to see
	 * if the collision mask has been fully cleared
	 */
	list_for_each_entry_safe(next_update, temp,
				&fb_data->upd_buf_collision_list->list, list) {

		if (next_update->collision_mask != 0)
			continue;

		dev_dbg(fb_data->dev, "A collision update is ready to go!\n");

		/*
		 * We have a collision cleared, so select it for resubmission.
		 * If an update is already selected, attempt to merge.
		 */
		if (!upd_data_list) {
			upd_data_list = next_update;
			list_del_init(&next_update->list);
			if (fb_data->upd_scheme == UPDATE_SCHEME_QUEUE)
				/* If not merging, we have our update */
				break;
		} else {
			switch (epdc_submit_merge(upd_data_list,
							next_update)) {
			case MERGE_OK:
				dev_dbg(fb_data->dev,
					"Update merged [collision]\n");
				list_del_init(&next_update->list);
				/* Add to free buffer list */
				list_add_tail(&next_update->list,
					 &fb_data->upd_buf_free_list->list);
/* 2011/04/19 FY11 : Supported wake lock. */
				epdc_wake_unlock_for_update_buffer( fb_data );
				break;
			case MERGE_FAIL:
				dev_dbg(fb_data->dev,
					"Update not merged [collision]\n");
				break;
			case MERGE_BLOCK:
				dev_dbg(fb_data->dev,
					"Merge blocked [collision]\n");
				end_merge = true;
				break;
			}

			if (end_merge) {
				end_merge = false;
				break;
			}
		}
	}

	/*
	 * Skip update queue only if we found a collision
	 * update and we are not merging
	 */
	if (!((fb_data->upd_scheme == UPDATE_SCHEME_QUEUE) &&
		upd_data_list)) {
		/*
		 * If we didn't find a collision update ready to go,
		 * we try to grab one from the update queue
		 */
		list_for_each_entry_safe(next_update, temp,
					&fb_data->upd_buf_queue->list, list) {

			dev_dbg(fb_data->dev, "Found a pending update!\n");

			if (!upd_data_list) {
				upd_data_list = next_update;
				list_del_init(&next_update->list);
				if (fb_data->upd_scheme == UPDATE_SCHEME_QUEUE)
					/* If not merging, we have an update */
					break;
			} else {
				switch (epdc_submit_merge(upd_data_list,
								next_update)) {
				case MERGE_OK:
					dev_dbg(fb_data->dev,
						"Update merged [queue]\n");
					list_del_init(&next_update->list);
					/* Add to free buffer list */
					list_add_tail(&next_update->list,
						 &fb_data->upd_buf_free_list->list);
/* 2011/04/19 FY11 : Supported wake lock. */
					epdc_wake_unlock_for_update_buffer( fb_data );
					break;
				case MERGE_FAIL:
					dev_dbg(fb_data->dev,
						"Update not merged [queue]\n");
					break;
				case MERGE_BLOCK:
					dev_dbg(fb_data->dev,
						"Merge blocked [collision]\n");
					end_merge = true;
					break;
				}

				if (end_merge)
					break;
			}
		}
	}

	/* Release buffer queues */
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	/* Is update list empty? */
	if (!upd_data_list)
		return;

	/* Perform PXP processing - EPDC power will also be enabled */
	if (epdc_process_update(upd_data_list, fb_data)) {
		dev_dbg(fb_data->dev, "PXP processing error.\n");
		/* Protect access to buffer queues and to update HW */
		spin_lock_irqsave(&fb_data->queue_lock, flags);
		/* Add to free buffer list */
		list_add_tail(&upd_data_list->list,
			 &fb_data->upd_buf_free_list->list);
/* 2011/04/19 FY11 : Supported wake lock. */
		epdc_wake_unlock_for_update_buffer( fb_data );
		/* Release buffer queues */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return;
	}

/* 2011/2/14 FY11 : Supported auto temperature reading. */
	mxc_epdc_fb_read_temperature(fb_data);

/*2011/07/05 FY11 : Modified to support A2 limitation. */
	/* If addition of updating screen is requested, wait completion of all pane update. */
	if ( upd_data_list->use_white_buf )
	{
		struct mxcfb_update_data inserted_update;
		u32 disp_val;

		inserted_update.update_region.left = 0;
		inserted_update.update_region.top = 0;
		inserted_update.update_region.width = fb_data->native_width;
		inserted_update.update_region.height = fb_data->native_height;
		inserted_update.waveform_mode = fb_data->wv_modes.mode_du;
		inserted_update.update_mode = UPDATE_MODE_PARTIAL;
		disp_val = 0xFF;
		epdc_debug_printk( "Draw inserted screen. (val=%d)\n", disp_val );
		ret = epdc_draw_rect(fb_data, &inserted_update, disp_val, true);
		if ( ret < 0 )
		{
			printk(KERN_ERR "%s Draw white screen failed. %d\n", __func__, ret );
		}
		else
		{
			epdc_debug_printk( "Draw white screen completed\n" );
		}
	}


	/* Get rotation-adjusted coordinates */
	adjust_coordinates(fb_data, &upd_data_list->upd_data.update_region,
		&adj_update_region);

	/* Protect access to buffer queues and to update HW */
	spin_lock_irqsave(&fb_data->queue_lock, flags);

	/*
	 * Is the working buffer idle?
	 * If the working buffer is busy, we must wait for the resource
	 * to become free. The IST will signal this event.
	 */
	if (fb_data->cur_update != NULL) {
		dev_dbg(fb_data->dev, "working buf busy!\n");

		/* Initialize event signalling an update resource is free */
		init_completion(&fb_data->update_res_free);

		fb_data->waiting_for_wb = true;

		/* Leave spinlock while waiting for WB to complete */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		wait_for_completion(&fb_data->update_res_free);
		spin_lock_irqsave(&fb_data->queue_lock, flags);
	}

	/*
	 * If there are no LUTs available,
	 * then we must wait for the resource to become free.
	 * The IST will signal this event.
	 */
	if (!epdc_any_luts_available()) {
		dev_dbg(fb_data->dev, "no luts available!\n");

		/* Initialize event signalling an update resource is free */
		init_completion(&fb_data->update_res_free);

		fb_data->waiting_for_lut = true;

		/* Leave spinlock while waiting for LUT to free up */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		wait_for_completion(&fb_data->update_res_free);
		spin_lock_irqsave(&fb_data->queue_lock, flags);
	}

	ret = epdc_choose_next_lut(&upd_data_list->lut_num);
	/*
	 * If LUT15 is in use:
	 *   - Wait for LUT15 to complete is if TCE underrun prevent is enabled
	 *   - If we go ahead with update, sync update submission with EOF
	 */
	if (ret && fb_data->tce_prevent) {
		dev_dbg(fb_data->dev, "Waiting for LUT15\n");

		/* Initialize event signalling that lut15 is free */
		init_completion(&fb_data->lut15_free);

		fb_data->waiting_for_lut15 = true;

		/* Leave spinlock while waiting for LUT to free up */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		wait_for_completion(&fb_data->lut15_free);
		spin_lock_irqsave(&fb_data->queue_lock, flags);

		epdc_choose_next_lut(&upd_data_list->lut_num);
	} else if (ret) {
		/* Synchronize update submission time to reduce
				chances of TCE underrun */
		init_completion(&fb_data->eof_event);

		epdc_eof_intr(true);

		/* Leave spinlock while waiting for EOF event */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		ret = wait_for_completion_timeout(&fb_data->eof_event,
			msecs_to_jiffies(1000));
		if (!ret) {
			dev_err(fb_data->dev, "Missed EOF event!\n");
			epdc_eof_intr(false);
		}
		udelay(fb_data->eof_sync_period);
		spin_lock_irqsave(&fb_data->queue_lock, flags);

	}

	/* LUTs are available, so we get one here */
	fb_data->cur_update = upd_data_list;

	/* Reset mask for LUTS that have completed during WB processing */
	fb_data->luts_complete_wb = 0;

	/* Associate LUT with update marker */
	if ((fb_data->cur_update->upd_marker_data)
		&& (fb_data->cur_update->upd_marker_data->update_marker != 0))
		fb_data->cur_update->upd_marker_data->lut_num =
						fb_data->cur_update->lut_num;

	/* Mark LUT with order */
	fb_data->lut_update_order[fb_data->cur_update->lut_num] =
		fb_data->cur_update->update_order;

	/* Enable Collision and WB complete IRQs */
	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(fb_data->cur_update->lut_num, true);

	/* Program EPDC update to process buffer */
	if (fb_data->cur_update->upd_data.temp != TEMP_USE_AMBIENT) {
		temp_index = mxc_epdc_fb_get_temp_index(fb_data,
				fb_data->cur_update->upd_data.temp);
		epdc_set_temp(temp_index);
	}
/* 2011/02/14 FY11 : Fixed the bug does not set current temperature. */
	else
	{
		epdc_set_temp(fb_data->temp_index);
	}

	epdc_set_update_addr(fb_data->cur_update->phys_addr
				+ fb_data->cur_update->epdc_offs);
	epdc_set_update_coord(adj_update_region.left, adj_update_region.top);
	epdc_set_update_dimensions(adj_update_region.width,
				   adj_update_region.height);
	epdc_submit_update(fb_data->cur_update->lut_num,
			   fb_data->cur_update->upd_data.waveform_mode,
			   fb_data->cur_update->upd_data.update_mode, false, 0);

	/* Release buffer queues */
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
}


/* 2011/03/15 FY11 : Supported standby screen. */
int mxc_epdc_fb_send_update(struct mxcfb_update_data *upd_data,
				   struct fb_info *info, bool bStandbyScreen )
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;
	struct update_data_list *upd_data_list = NULL;
	unsigned long flags;
	int i;
	struct mxcfb_rect *screen_upd_region; /* Region on screen to update */
	int temp_index;
	int ret;

	/* Has EPDC HW been initialized? */
	if (!fb_data->hw_ready) {
		dev_err(fb_data->dev, "Display HW not properly initialized."
			"  Aborting update.\n");
		return -EPERM;
	}

	/* Check validity of update params */
	if ((upd_data->update_mode != UPDATE_MODE_PARTIAL) &&
		(upd_data->update_mode != UPDATE_MODE_FULL)) {
		dev_err(fb_data->dev,
			"Update mode 0x%x is invalid.  Aborting update.\n",
			upd_data->update_mode);
		return -EINVAL;
	}
	if ((upd_data->waveform_mode > 255) &&
		(upd_data->waveform_mode != WAVEFORM_MODE_AUTO)) {
		dev_err(fb_data->dev,
			"Update waveform mode 0x%x is invalid."
			"  Aborting update.\n",
			upd_data->waveform_mode);
		return -EINVAL;
	}
	if ((upd_data->update_region.left + upd_data->update_region.width > fb_data->epdc_fb_var.xres) ||
		(upd_data->update_region.top + upd_data->update_region.height > fb_data->epdc_fb_var.yres)) {
		dev_err(fb_data->dev,
			"Update region is outside bounds of framebuffer."
			"Aborting update.\n");
		return -EINVAL;
	}
	if ((upd_data->flags & EPDC_FLAG_USE_ALT_BUFFER) &&
		((upd_data->update_region.width != upd_data->alt_buffer_data.alt_update_region.width) ||
		(upd_data->update_region.height != upd_data->alt_buffer_data.alt_update_region.height))) {
		dev_err(fb_data->dev,
			"Alternate update region dimensions must match screen update region dimensions.\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&fb_data->queue_lock, flags);

	/*
	 * If we are waiting to go into suspend, or the FB is blanked,
	 * we do not accept new updates
	 */
	if ((fb_data->waiting_for_idle) ||
		(fb_data->blank != FB_BLANK_UNBLANK)) {
		dev_dbg(fb_data->dev, "EPDC not active."
			"Update request abort.\n");
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return -EPERM;
	}

	/*
	 * Get available intermediate (PxP output) buffer to hold
	 * processed update region
	 */
	if (list_empty(&fb_data->upd_buf_free_list->list)) {
		dev_err(fb_data->dev,
			"No free intermediate buffers available.\n");
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return -ENOMEM;
	}

/* 2011/07/05 FY11 : Fixed A2 limitation. */
	if (	(	( fb_data->force_wf_mode == false )
		     && ( upd_data->waveform_mode == fb_data->wv_modes.mode_a2 )
		)
	     || (	( fb_data->force_wf_mode == true ) 
		     && ( upd_data->waveform_mode == fb_data->wv_modes.mode_gc16 ) 
		     && ( upd_data->update_mode == UPDATE_MODE_FULL )
		)
	   )
	{
		// wait all update completion.
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		epdc_debug_printk( "wait previous updates completion\n" );
		mxc_epdc_fb_flush_updates(fb_data);
		spin_lock_irqsave(&fb_data->queue_lock, flags);
	}

	
	/* Grab first available buffer and delete it from the free list */
	upd_data_list =
	    list_entry(fb_data->upd_buf_free_list->list.next,
		       struct update_data_list, list);

/* 2011/04/19 FY11 : Supported wake lock. */
	epdc_wake_lock_for_update_buffer( fb_data );
	list_del_init(&upd_data_list->list);

	/* copy update parameters to the current update data object */
	memcpy(&upd_data_list->upd_data, upd_data,
	       sizeof(struct mxcfb_update_data));
	memcpy(&upd_data_list->upd_data.update_region, &upd_data->update_region,
	       sizeof(struct mxcfb_rect));
/* 2011/03/05 FY11 : Supported A2 mode limitations. */
	upd_data_list->use_white_buf = 0;
	if (	(	( fb_data->force_wf_mode == false )
		     && ( upd_data->waveform_mode == fb_data->wv_modes.mode_a2 )
		)
	     || (	( fb_data->force_wf_mode == true ) 
		     && ( upd_data->waveform_mode == fb_data->wv_modes.mode_gc16 ) 
		     && ( upd_data->update_mode == UPDATE_MODE_FULL )
		)
	   )
	{
		upd_data_list->use_white_buf = 1;

		if ( fb_data->force_wf_mode == true )
		{
			/* modify update area. */
			upd_data->update_region.top = 0;
			upd_data->update_region.left = 0;
			upd_data->update_region.width = fb_data->info.var.xres;
			upd_data->update_region.height = fb_data->info.var.yres;

/* 2011/07/14 FY11 : Added wake lock for A2. (Workaround for noise after sleep.) */
			wake_unlock( &(fb_data->wake_lock_a2) );
		}
		else
		{
/* 2011/07/14 FY11 : Added wake lock for A2. (Workaround for noise after sleep.) */
			wake_lock( &(fb_data->wake_lock_a2) );
		}

		/* Set force wf mode flag */
		fb_data->force_wf_mode = !(fb_data->force_wf_mode);
	}


	/* If marker specified, associate it with a completion */
	if (upd_data->update_marker != 0) {
		/* Find available update marker and set it up */
		for (i = 0; i < EPDC_MAX_NUM_UPDATES; i++) {
			/* Marker value set to 0 signifies it is not currently in use */
			if (fb_data->update_marker_array[i].update_marker == 0) {
				fb_data->update_marker_array[i].update_marker = upd_data->update_marker;
				init_completion(&fb_data->update_marker_array[i].update_completion);
				upd_data_list->upd_marker_data = &fb_data->update_marker_array[i];
				break;
			}
		}
	} else {
		if (upd_data_list->upd_marker_data)
			upd_data_list->upd_marker_data->update_marker = 0;
	}

	upd_data_list->update_order = fb_data->order_cnt++;


/* 2011/03/15 FY11 : Supported standby screen. */
	if ( bStandbyScreen )
	{
		upd_data_list->use_standbyscreen = true;
	}
	else
	{
		upd_data_list->use_standbyscreen = false;
	}

	if (fb_data->upd_scheme != UPDATE_SCHEME_SNAPSHOT) {
		/* Queued update scheme processing */

		/* Add processed Y buffer to update list */
		list_add_tail(&upd_data_list->list,
				  &fb_data->upd_buf_queue->list);

		spin_unlock_irqrestore(&fb_data->queue_lock, flags);

		/* Signal workqueue to handle new update */
		queue_work(fb_data->epdc_submit_workqueue,
			&fb_data->epdc_submit_work);

		return 0;
	}

	/* Snapshot update scheme processing */

	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	/*
	 * Hold on to original screen update region, which we
	 * will ultimately use when telling EPDC where to update on panel
	 */
	screen_upd_region = &upd_data_list->upd_data.update_region;

	ret = epdc_process_update(upd_data_list, fb_data);
	if (ret) {
		mutex_unlock(&fb_data->pxp_mutex);
		return ret;
	}

/* 2011/2/14 FY11 : Supported auto temperature reading. */
	mxc_epdc_fb_read_temperature(fb_data);

	/* Pass selected waveform mode back to user */
	upd_data->waveform_mode = upd_data_list->upd_data.waveform_mode;

	/* Get rotation-adjusted coordinates */
	adjust_coordinates(fb_data, &upd_data_list->upd_data.update_region,
		NULL);

	/* Grab lock for queue manipulation and update submission */
	spin_lock_irqsave(&fb_data->queue_lock, flags);

	/*
	 * Is the working buffer idle?
	 * If either the working buffer is busy, or there are no LUTs available,
	 * then we return and let the ISR handle the update later
	 */
	if ((fb_data->cur_update != NULL) || !epdc_any_luts_available()) {
		/* Add processed Y buffer to update list */
		list_add_tail(&upd_data_list->list,
			      &fb_data->upd_buf_queue->list);

		/* Return and allow the update to be submitted by the ISR. */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return 0;
	}

	/* LUTs are available, so we get one here */
	ret = epdc_choose_next_lut(&upd_data_list->lut_num);
	if (ret && fb_data->tce_prevent) {
		dev_dbg(fb_data->dev, "Must wait for LUT15\n");
		/* Add processed Y buffer to update list */
		list_add_tail(&upd_data_list->list, &fb_data->upd_buf_queue->list);

		/* Return and allow the update to be submitted by the ISR. */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return 0;
	}

	/* Reset mask for LUTS that have completed during WB processing */
	fb_data->luts_complete_wb = 0;

	/* Save current update */
	fb_data->cur_update = upd_data_list;

	/* Associate LUT with update marker */
	if (upd_data_list->upd_marker_data)
		if (upd_data_list->upd_marker_data->update_marker != 0)
			upd_data_list->upd_marker_data->lut_num = upd_data_list->lut_num;

	/* Mark LUT as containing new update */
	fb_data->lut_update_order[upd_data_list->lut_num] =
		upd_data_list->update_order;

	/* Clear status and Enable LUT complete and WB complete IRQs */
	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(fb_data->cur_update->lut_num, true);

	/* Program EPDC update to process buffer */
	epdc_set_update_addr(upd_data_list->phys_addr + upd_data_list->epdc_offs);
	epdc_set_update_coord(screen_upd_region->left, screen_upd_region->top);
	epdc_set_update_dimensions(screen_upd_region->width,
		screen_upd_region->height);
	if (upd_data_list->upd_data.temp != TEMP_USE_AMBIENT) {
		temp_index = mxc_epdc_fb_get_temp_index(fb_data,
			upd_data_list->upd_data.temp);
		epdc_set_temp(temp_index);
	} else
		epdc_set_temp(fb_data->temp_index);

	epdc_submit_update(upd_data_list->lut_num,
			   upd_data_list->upd_data.waveform_mode,
			   upd_data_list->upd_data.update_mode, false, 0);

	spin_unlock_irqrestore(&fb_data->queue_lock, flags);
	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_send_update);

int mxc_epdc_fb_wait_update_complete(u32 update_marker, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;
	int ret = 0;
	int i, retry;

/* 2011/06/13 FY11 : Added the function to wait all update requests. */
	/* 0 means to wait all update requests. */
	if (update_marker == 0)
	{
		epdc_debug_printk( "wait all update request's completion\n" );

		// wait starting of all panel update requested by application.
		mxc_epdc_fb_flush_updates(fb_data);

		/* Enable clocks to EPDC */
		mutex_lock(&fb_data->power_mutex);
		clk_enable(fb_data->epdc_clk_axi);
		clk_enable(fb_data->epdc_clk_pix);
		epdc_clock_gating(false);
		mutex_unlock(&fb_data->power_mutex);

		epdc_debug_printk( "wait each lut completion\n" );
		// wait all update completion.
		for ( i = 0; i < EPDC_NUM_LUTS; i++ )
		{
			retry = 30;
			while( epdc_is_lut_active(i) ||
				epdc_is_working_buffer_busy() )
			{
				epdc_debug_printk( "wait lut %d completion\n", i );
				msleep(100);
				retry--;
				if ( retry == 0 )
				{
					printk( KERN_ERR "%s wait lut %d idle timeout.\n", __func__, i );
					ret = -ETIMEDOUT;
					break;
				}
			}
		}

		/* disable clocks to EPDC */
		mutex_lock(&fb_data->power_mutex);
		epdc_clock_gating(true);
		clk_disable(fb_data->epdc_clk_axi);
		clk_disable(fb_data->epdc_clk_pix);
		mutex_unlock(&fb_data->power_mutex);

		return ret;		
	}

	/*
	 * Wait for completion associated with update_marker requested.
	 * Note: If update completed already, marker will have been
	 * cleared and we will just return
	 */
	epdc_debug_printk( "wait update request(marker=0x%08X\n", update_marker );
	for (i = 0; i < EPDC_MAX_NUM_UPDATES; i++) {
		if (fb_data->update_marker_array[i].update_marker == update_marker) {
			dev_dbg(fb_data->dev, "Waiting for marker %d\n", update_marker);
			ret = wait_for_completion_timeout(&fb_data->update_marker_array[i].update_completion, msecs_to_jiffies(5000));
			if (!ret)
				dev_err(fb_data->dev, "Timed out waiting for update completion\n");
			dev_dbg(fb_data->dev, "marker %d signalled!\n", update_marker);
			break;
		}
	}

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_wait_update_complete);

int mxc_epdc_fb_set_pwrdown_delay(u32 pwrdown_delay,
					    struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	fb_data->pwrdown_delay = pwrdown_delay;

	return 0;
}
EXPORT_SYMBOL(mxc_epdc_fb_set_pwrdown_delay);

int mxc_epdc_get_pwrdown_delay(struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = info ?
		(struct mxc_epdc_fb_data *)info:g_fb_data;

	return fb_data->pwrdown_delay;
}
EXPORT_SYMBOL(mxc_epdc_get_pwrdown_delay);

static int mxc_epdc_fb_set_pwr0_ctrl(u32 ctrl,
					    struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;

	if (ctrl)
	  regulator_enable(fb_data->epd_pwr0_regulator);
	else
	  regulator_disable(fb_data->epd_pwr0_regulator);

	return 0;
}

static int mxc_epdc_fb_set_pwr2_ctrl(u32 ctrl,
					    struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;

	if (ctrl)
	    regulator_enable(fb_data->epd_pwr2_regulator);
	else
	    regulator_disable(fb_data->epd_pwr2_regulator);
	return 0;
}


/* 2011/1/19 FY11 : Added function to read/write VCOM. */
int mxc_epdc_fb_set_vcom_voltage( u32 uvoltage, struct fb_info *info)
{
	int ret = 0;
	int savedPowerState;
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data*)info;

	if ((savedPowerState = fb_data->power_state) == POWER_STATE_OFF)
		regulator_enable(fb_data->display_regulator);

	ret = regulator_set_voltage(fb_data->vcom_regulator, 0, uvoltage);
	if ( ret < 0 )
	{
		printk( KERN_ERR "Fail to set vcom voltage!! %d\n", ret );
	}

	if (savedPowerState == POWER_STATE_OFF)
		regulator_disable(fb_data->display_regulator);

	return ret;
}

/* 2011/1/19 FY11 : Added function to read/write VCOM. */
int mxc_epdc_fb_get_vcom_voltage( u32* uvoltage, struct fb_info *info)
{
	int ret = 0;
	int savedPowerState;
	struct mxc_epdc_fb_data *fb_data;

	if ( uvoltage == NULL )
	{
		return -EINVAL;
	}
	fb_data = (struct mxc_epdc_fb_data*)info;

	if ((savedPowerState = fb_data->power_state) == POWER_STATE_OFF)
		regulator_enable(fb_data->display_regulator);


	ret = regulator_get_voltage(fb_data->vcom_regulator);
	if ( ret < 0 )
	{
		printk( KERN_ERR "Fail to get vcom voltage. %d\n", ret );
	}
	else
	{
		*uvoltage = (u32)ret;
		ret = 0;	/* 2011/04/14 FY11 : Fixed invalid return value. */
	}

	if (savedPowerState == POWER_STATE_OFF)
		regulator_disable(fb_data->display_regulator);

	return ret;
}


/* 2011/03/08 FY11 : Added command to write waveform. */
static int mxc_epdc_fb_write_wf( struct mxcfb_waveform_data *wf_data,
				 struct fb_info *fbinfo )
{
	int ret = 0;
	int i, wf_data_size;
	unsigned long ulWfIndex, ulWfAreaSize;

	ulWfIndex = rawdata_index("Waveform", &ulWfAreaSize);
	if ( (ulWfAreaSize - sizeof(struct epd_settings) - sizeof(unsigned long) ) < wf_data->uiSize )
	{
		printk( KERN_ERR "Waveform is too large !!\n" );
		return -EINVAL;
	}

/* 2011/06/08 FY11 : Modified to use static buffer for waveform. */
	wf_data_size = (wf_data->uiSize);
	for ( i = 0; wf_data_size > 0; i++ )
	{
		ret = copy_from_user( 	wf_tmp_buf, 
					wf_data->pcData + i * WF_TMP_BUF_SIZE, 
					wf_data_size > WF_TMP_BUF_SIZE ? WF_TMP_BUF_SIZE : wf_data_size);
		if ( ret < 0 )
		{
			printk( KERN_ERR "%s fail to read wf from user. %d\n", __func__, ret );
			goto out;
		}

		ret = rawdata_write( ulWfIndex, i*WF_TMP_BUF_SIZE,
				     wf_tmp_buf,
				     wf_data_size > WF_TMP_BUF_SIZE ? WF_TMP_BUF_SIZE : wf_data_size);
		if ( ret < 0 )
		{
			printk( KERN_ERR "%s fail to write wf. %d\n", __func__, ret );
			goto out;
		}

		wf_data_size -= WF_TMP_BUF_SIZE;
	}

out:
	return ret;
}


/* 2011/04/12 FY11 : Supported to write panel init flag. */
static int mxc_epdc_set_panel_init_flag ( unsigned char ucFlag, struct mxc_epdc_fb_data *fb_data )
{
	int ret = 0;
	unsigned long ulWfIndex, ulWfAreaSize;
	unsigned long epd_setting_flag;

	ulWfIndex = rawdata_index("Waveform", &ulWfAreaSize);
	ret = rawdata_read( ulWfIndex, 
			    ulWfAreaSize - sizeof(struct epd_settings) + offsetof(struct epd_settings, flag),
			    (char*)&epd_setting_flag,
			    sizeof(epd_setting_flag) );
	if ( ret < 0 )
	{
		printk ( KERN_ERR "%s fail to read flag from eMMC.\n", __func__ );
	}
	else
	{
		if (ucFlag)
		{
			epd_setting_flag |= EPD_SETTING_PANEL_INIT;
			fb_data->panel_clear_at_shutdown = false;
		}
		else
		{
			epd_setting_flag &= ~EPD_SETTING_PANEL_INIT;
			fb_data->panel_clear_at_shutdown = true;
		}
		ret = rawdata_write( ulWfIndex, 
				    ulWfAreaSize - sizeof(struct epd_settings) + offsetof(struct epd_settings, flag),
				    (char*)&epd_setting_flag,
				    sizeof(epd_setting_flag) );
		if ( ret < 0 )
		{
			printk ( KERN_ERR "%s fail to write flag to eMMC.\n", __func__ );
		}
	}

	return ret;
}



static int mxc_epdc_fb_ioctl(struct fb_info *info, unsigned int cmd,
			     unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int ret = -EINVAL;


/* 2011/04/19 FY11 : Supported wake lock. */
	struct mxc_epdc_fb_data *fb_data_ioctl =
			(struct mxc_epdc_fb_data *)info;
	wake_lock(&(fb_data_ioctl->wake_lock_ioctl));


	switch (cmd) {
	case MXCFB_SET_WAVEFORM_MODES:
		{
			struct mxcfb_waveform_modes modes;
			if (!copy_from_user(&modes, argp, sizeof(modes))) {
				mxc_epdc_fb_set_waveform_modes(&modes, info);
				ret = 0;
			}
			break;
		}
	case MXCFB_SET_TEMPERATURE:
		{
			int temperature;
			if (!get_user(temperature, (int32_t __user *) arg))
				ret = mxc_epdc_fb_set_temperature(temperature,
					info);
			break;
		}
	case MXCFB_SET_AUTO_UPDATE_MODE:
		{
			u32 auto_mode = 0;
			if (!get_user(auto_mode, (__u32 __user *) arg))
				ret = mxc_epdc_fb_set_auto_update(auto_mode,
					info);
			break;
		}
	case MXCFB_SET_UPDATE_SCHEME:
		{
			u32 upd_scheme = 0;
			if (!get_user(upd_scheme, (__u32 __user *) arg))
				ret = mxc_epdc_fb_set_upd_scheme(upd_scheme,
					info);
			break;
		}
	case MXCFB_SEND_UPDATE:
		{
			struct mxcfb_update_data upd_data;
			if (!copy_from_user(&upd_data, argp,
				sizeof(upd_data))) {
				ret = mxc_epdc_fb_send_update(&upd_data, info, false);
				if (ret == 0 && copy_to_user(argp, &upd_data,
					sizeof(upd_data)))
					ret = -EFAULT;
			} else {
				ret = -EFAULT;
			}

			break;
		}
	case MXCFB_WAIT_FOR_UPDATE_COMPLETE:
		{
			u32 update_marker = 0;
			if (!get_user(update_marker, (__u32 __user *) arg))
				ret =
				    mxc_epdc_fb_wait_update_complete(update_marker,
					info);
			break;
		}

	case MXCFB_SET_PWRDOWN_DELAY:
		{
			int delay = 0;
			if (!get_user(delay, (__u32 __user *) arg))
				ret =
				    mxc_epdc_fb_set_pwrdown_delay(delay, info);
			break;
		}

	case MXCFB_GET_PWRDOWN_DELAY:
		{
			int pwrdown_delay = mxc_epdc_get_pwrdown_delay(info);
			if (put_user(pwrdown_delay,
				(int __user *)argp))
				ret = -EFAULT;
			ret = 0;
			break;
		}

/* 2011/1/19 FY11 : Added function to read/write VCOM. */
	case MXCFB_SET_VCOM:
		{
			u32 uvoltage = 0;
			if (!get_user(uvoltage, (__u32 __user *) arg))
				ret = mxc_epdc_fb_set_vcom_voltage(uvoltage, info);
			break;
		}
	case MXCFB_GET_VCOM:
		{
			u32 uvoltage = 0;
			ret = mxc_epdc_fb_get_vcom_voltage(&uvoltage, info);
			if ( ret < 0 )
			{
				break;
			}
			if (put_user(uvoltage,(__u32 __user *)argp))
			{
				ret = -EFAULT;
			}
			break;
		}

	case MXCFB_GET_PMIC_TEMPERATURE:
		{
			struct mxc_epdc_fb_data *fb_data =
				(struct mxc_epdc_fb_data *)info;
			
			int temperature;
			int savedPowerState;

/* 2011/2/3 FY11: Fixed the bug that could not disable power after reading temperature.*/
			if ((savedPowerState = fb_data->power_state) == POWER_STATE_OFF)
				regulator_enable(fb_data->display_regulator);
			temperature = regulator_get_voltage(fb_data->temp_regulator);
			if (savedPowerState == POWER_STATE_OFF)
				regulator_disable(fb_data->display_regulator);

			ret = 0;
#if 1 /* E_BOOK */
/* 2010/12/10 FY11: Fixed the bug to return correct temperature.*/
			if (temperature < 0 )	// err
			{
				ret = temperature;
			}
			else 
			{
				if ( temperature >= MINUS_TEMPERATURE )
				{
					temperature -= MINUS_TEMP_ADJUST_VAL;
				}
	
				if (put_user(temperature,(int __user *)argp))
				{
					ret = -EFAULT;
				}
			}
#endif /* E_BOOK */
			break;
		}

	case MXCFB_SET_BORDER_MODE:
		{
			int mode;
			if (!get_user(mode, (int32_t __user *) arg))
				ret =
				    mxc_epdc_fb_set_border_mode(mode,
					info);
			break;
		}
	case MXCFB_SET_EPD_PWR0_CTRL:
		{
			int control = 0;
			if (!get_user(control, (__u32 __user *) arg))
				ret =
				    mxc_epdc_fb_set_pwr0_ctrl(control, info);
			break;
		}

	case MXCFB_SET_EPD_PWR2_CTRL:
		{
			int control = 0;
			if (!get_user(control, (__u32 __user *) arg))
				ret =
				    mxc_epdc_fb_set_pwr2_ctrl(control, info);
			break;
		}
/* 2011/2/24 FY11 : Supported to read waveform version. */
	case MXCFB_GET_WF_VERSION:
		{
			ret = copy_to_user(argp, &(s_st_wfversion.version[0]), WF_VER_LEN );
			if ( ret < 0 )
			{
				printk( KERN_ERR "copy waveform version failed. %d\n", ret );
			}
			break;
		}
/* 2011/03/08 FY11 : Added command to write waveform. */
	case MXCFB_WRITE_WF:
		{
			struct mxc_epdc_fb_data *fb_data =
				(struct mxc_epdc_fb_data *)info;
			struct mxcfb_waveform_data wf_data;
			ret = copy_from_user(&wf_data, argp, sizeof(wf_data) );
			if ( ret != 0 )
			{
				printk( KERN_ERR "Fail to read waveform from user.\n" );
				ret = -EFAULT;
			}

			/* wait for completion of all updates */
			flush_workqueue(fb_data->epdc_submit_workqueue);

			/* write waveform to eMMC. */
			ret = mxc_epdc_fb_write_wf(&wf_data, info );
			if ( ret < 0 )
			{
				printk( KERN_ERR "Fail to write waveform to eMMC.\n" );
			}

			/* reload waveform */
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
			ret = mxc_epdc_fb_load_wf( fb_data, true );
			if ( ret < 0 )
			{
				printk( KERN_ERR "Fail to reload waveform from eMMC and to init EPDC.\n" );
			}

			/* panel init */
/* 2011/04/06 FY11 : Modified to call panel init independently. (Not included in mxc_epdc_fb_load_wf) */
			draw_mode0( fb_data );
			break;
		}
/* 2011/03/30 FY11 : Supported to write stanby screen image. */
	case MXCFB_WRITE_SSCREEN:
		{
			struct mxc_epdc_fb_data *fb_data =
				(struct mxc_epdc_fb_data *)info;
			char *pBuf = NULL;
			int line = 0;
			int byteperpix = info->var.bits_per_pixel/8;
			ret = copy_from_user(&pBuf, argp, sizeof(char*) );
			if ( ret != 0 )
			{
				printk( KERN_ERR "Fail to read buffer pointer from user.\n" );
				ret = -EFAULT;
			}

			for ( line = 0; line < info->var.yres; line++ )
			{
				ret = copy_from_user( (fb_data->standbyscreen_buff_virt) + line*(info->var.xres_virtual)*byteperpix,
							pBuf + line*(info->var.xres)*byteperpix,
							(info->var.xres) * byteperpix );
				if ( ret < 0 )
				{
					printk( KERN_ERR "Fail to read standby screen image from user. %d\n", ret );
					break;
				}
			}

			break;
		}
/* 2011/04/12 FY11 : Supported to write panel init flag. */
	case MXCFB_SET_PANELINIT:
		{
			struct mxc_epdc_fb_data *fb_data =
				(struct mxc_epdc_fb_data *)info;
			unsigned char ucFlag;
			ret = copy_from_user(&ucFlag, argp, sizeof(unsigned char) );
			if ( ret != 0 )
			{
				printk( KERN_ERR "Fail to read Flag from user.\n" );
				ret = -EFAULT;
				break;
			}
			ret = mxc_epdc_set_panel_init_flag(ucFlag, fb_data);
			break;
		}
	default:
		break;
	}

/* 2011/04/19 FY11 : Supported wake lock. */
	wake_unlock(&(fb_data_ioctl->wake_lock_ioctl));

	return ret;
}

static void mxc_epdc_fb_update_pages(struct mxc_epdc_fb_data *fb_data,
				     u16 y1, u16 y2)
{
	struct mxcfb_update_data update;

	/* Do partial screen update, Update full horizontal lines */
	update.update_region.left = 0;
	update.update_region.width = fb_data->epdc_fb_var.xres;
	update.update_region.top = y1;
	update.update_region.height = y2 - y1;
	update.waveform_mode = WAVEFORM_MODE_AUTO;
	update.update_mode = UPDATE_MODE_FULL;
	update.update_marker = 0;
	update.temp = TEMP_USE_AMBIENT;
	update.flags = 0;

	mxc_epdc_fb_send_update(&update, &fb_data->info, false);
}

/* this is called back from the deferred io workqueue */
static void mxc_epdc_fb_deferred_io(struct fb_info *info,
				    struct list_head *pagelist)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	struct page *page;
	unsigned long beg, end;
	int y1, y2, miny, maxy;

	if (fb_data->auto_mode != AUTO_UPDATE_MODE_AUTOMATIC_MODE)
		return;

	miny = INT_MAX;
	maxy = 0;
	list_for_each_entry(page, pagelist, lru) {
		beg = page->index << PAGE_SHIFT;
		end = beg + PAGE_SIZE - 1;
		y1 = beg / info->fix.line_length;
		y2 = end / info->fix.line_length;
		if (y2 >= fb_data->epdc_fb_var.yres)
			y2 = fb_data->epdc_fb_var.yres - 1;
		if (miny > y1)
			miny = y1;
		if (maxy < y2)
			maxy = y2;
	}

	mxc_epdc_fb_update_pages(fb_data, miny, maxy);
}

void mxc_epdc_fb_flush_updates(struct mxc_epdc_fb_data *fb_data)
{
	unsigned long flags;
	int ret;

	clk_enable(fb_data->epdc_clk_axi);
	epdc_clock_gating(false);       // enable clock

	/* Grab queue lock to prevent any new updates from being submitted */
	spin_lock_irqsave(&fb_data->queue_lock, flags);

/* 2011/07/15 FY11 : Added checking of active luts. */
	if ( (!is_free_list_full(fb_data)) ||
		epdc_any_luts_active() ) 
	{
		/* Initialize event signalling updates are done */
		init_completion(&fb_data->updates_done);
		fb_data->waiting_for_idle = true;

		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		/* Wait for any currently active updates to complete */
		ret = wait_for_completion_timeout(&fb_data->updates_done, msecs_to_jiffies(10000));
		if(!ret)
		{
			printk(KERN_ERR "%s wait timeout. ret=%d\n", __func__, ret );
		}

		spin_lock_irqsave(&fb_data->queue_lock, flags);
		fb_data->waiting_for_idle = false;
	}

	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	epdc_clock_gating(true);
	clk_disable(fb_data->epdc_clk_axi);
}

static int mxc_epdc_fb_blank(int blank, struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;

	dev_dbg(fb_data->dev, "blank = %d\n", blank);

	if (fb_data->blank == blank)
		return 0;

	fb_data->blank = blank;

	switch (blank) {
	case FB_BLANK_POWERDOWN:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_NORMAL:
		mxc_epdc_fb_flush_updates(fb_data);
		break;
	}
	return 0;
}

static int mxc_epdc_fb_pan_display(struct fb_var_screeninfo *var,
				   struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	u_int y_bottom;
	unsigned long flags;

	dev_dbg(info->device, "%s: var->yoffset %d, info->var.yoffset %d\n",
		 __func__, var->yoffset, info->var.yoffset);
	/* check if var is valid; also, xpan is not supported */
	if (!var || (var->xoffset != info->var.xoffset) ||
	    (var->yoffset + var->yres > var->yres_virtual)) {
		dev_dbg(info->device, "x panning not supported\n");
		return -EINVAL;
	}

	if ((fb_data->epdc_fb_var.xoffset == var->xoffset) &&
		(fb_data->epdc_fb_var.yoffset == var->yoffset))
		return 0;	/* No change, do nothing */

	y_bottom = var->yoffset;

	if (!(var->vmode & FB_VMODE_YWRAP))
		y_bottom += var->yres;

	if (y_bottom > info->var.yres_virtual) {
		return -EINVAL;
	}

	/* Display panning should be done when PXP doesn't access
	 * the frame buffer
	 */
	mutex_lock(&fb_data->pxp_mutex);

	spin_lock_irqsave(&fb_data->queue_lock, flags);

	fb_data->fb_offset = (var->yoffset * var->xres_virtual + var->xoffset)
		* (var->bits_per_pixel) / 8;

	fb_data->epdc_fb_var.xoffset = var->xoffset;
	fb_data->epdc_fb_var.yoffset = var->yoffset;

	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;

	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	mutex_unlock(&fb_data->pxp_mutex);

	return 0;
}

static struct fb_ops mxc_epdc_fb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = mxc_epdc_fb_check_var,
	.fb_set_par = mxc_epdc_fb_set_par,
	.fb_setcolreg = mxc_epdc_fb_setcolreg,
	.fb_pan_display = mxc_epdc_fb_pan_display,
	.fb_ioctl = mxc_epdc_fb_ioctl,
	.fb_mmap = mxc_epdc_fb_mmap,
	.fb_blank = mxc_epdc_fb_blank,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

static struct fb_deferred_io mxc_epdc_fb_defio = {
	.delay = HZ,
	.deferred_io = mxc_epdc_fb_deferred_io,
};

static void epdc_done_work_func(struct work_struct *work)
{
	struct mxc_epdc_fb_data *fb_data =
		container_of(work, struct mxc_epdc_fb_data,
			epdc_done_work.work);
	epdc_powerdown(fb_data);
}

static bool is_free_list_full(struct mxc_epdc_fb_data *fb_data)
{
	int count = 0;
	struct update_data_list *plist;

	/* Count buffers in free buffer list */
	list_for_each_entry(plist, &fb_data->upd_buf_free_list->list, list)
		count++;

	/* Check to see if all buffers are in this list */
	if (count == EPDC_MAX_NUM_UPDATES)
		return true;
	else
		return false;
}

static bool do_updates_overlap(struct update_data_list *update1,
					struct update_data_list *update2)
{
	struct mxcfb_rect *rect1 = &update1->upd_data.update_region;
	struct mxcfb_rect *rect2 = &update2->upd_data.update_region;
	__u32 bottom1, bottom2, right1, right2;
	bottom1 = rect1->top + rect1->height;
	bottom2 = rect2->top + rect2->height;
	right1 = rect1->left + rect1->width;
	right2 = rect2->left + rect2->width;

	if ((rect1->top < bottom2) &&
		(bottom1 > rect2->top) &&
		(rect1->left < right2) &&
		(right1 > rect2->left)) {
		return true;
	} else
		return false;
}
static irqreturn_t mxc_epdc_irq_handler(int irq, void *dev_id)
{
	struct mxc_epdc_fb_data *fb_data = dev_id;
	struct update_data_list *collision_update;
	struct mxcfb_rect *next_upd_region;
	unsigned long flags;
	int temp_index;
	u32 temp_mask;
	u32 missed_coll_mask = 0;
	u32 lut;
	bool ignore_collision = false;
	int i, j;
	int ret, next_lut;

	/*
	 * If we just completed one-time panel init, bypass
	 * queue handling, clear interrupt and return
	 */
	if (fb_data->in_init) {
		if (epdc_is_working_buffer_complete()) {
			epdc_working_buf_intr(false);
			epdc_clear_working_buf_irq();
			dev_dbg(fb_data->dev, "Cleared WB for init update\n");
		}

		if (epdc_is_lut_complete(0)) {
			epdc_lut_complete_intr(0, false);
			epdc_clear_lut_complete_irq(0);
			fb_data->in_init = false;
			dev_dbg(fb_data->dev, "Cleared LUT complete for init update\n");
		}

		return IRQ_HANDLED;
	}

	if (!(__raw_readl(EPDC_IRQ_MASK) & __raw_readl(EPDC_IRQ)))
		return IRQ_HANDLED;

	if (__raw_readl(EPDC_IRQ) & EPDC_IRQ_TCE_UNDERRUN_IRQ) {
		dev_err(fb_data->dev,
			"TCE underrun! Will continue to update panel\n");
		/* Clear TCE underrun IRQ */
		__raw_writel(EPDC_IRQ_TCE_UNDERRUN_IRQ, EPDC_IRQ_CLEAR);
	}

	/* Check if we are waiting on EOF to sync a new update submission */
	if (epdc_signal_eof()) {
		epdc_eof_intr(false);
		epdc_clear_eof_irq();
		complete(&fb_data->eof_event);
	}

	/* Protect access to buffer queues and to update HW */
	spin_lock_irqsave(&fb_data->queue_lock, flags);

	/* Free any LUTs that have completed */
	for (i = 0; i < EPDC_NUM_LUTS; i++) {
		if (!epdc_is_lut_complete(i))
			continue;

		dev_dbg(fb_data->dev, "\nLUT %d completed\n", i);

		/* Disable IRQ for completed LUT */
		epdc_lut_complete_intr(i, false);

		/*
		 * Go through all updates in the collision list and
		 * unmask any updates that were colliding with
		 * the completed LUT.
		 */
		list_for_each_entry(collision_update,
				    &fb_data->upd_buf_collision_list->
				    list, list) {
			collision_update->collision_mask =
			    collision_update->collision_mask & ~(1 << i);
		}

		epdc_clear_lut_complete_irq(i);

		fb_data->luts_complete_wb |= 1 << i;

		fb_data->lut_update_order[i] = 0;

		/* Signal completion if submit workqueue needs a LUT */
		if (fb_data->waiting_for_lut) {
			complete(&fb_data->update_res_free);
			fb_data->waiting_for_lut = false;
		}

		/* Signal completion if LUT15 free and is needed */
		if (fb_data->waiting_for_lut15 && (i == 15)) {
			complete(&fb_data->lut15_free);
			fb_data->waiting_for_lut15 = false;
		}

		/* Signal completion if anyone waiting on this LUT */
		for (j = 0; j < EPDC_MAX_NUM_UPDATES; j++) {
			if (fb_data->update_marker_array[j].lut_num != i)
				continue;

			/* Signal completion of update */
			dev_dbg(fb_data->dev,
				"Signaling marker %d\n",
				fb_data->update_marker_array[j].update_marker);
			complete(&fb_data->update_marker_array[j].update_completion);
			/* Ensure this doesn't get signaled again inadvertently */
			fb_data->update_marker_array[j].lut_num = INVALID_LUT;
			/*
			 * Setting marker to 0 is OK - any wait call will
			 * return when marker doesn't match any in array
			 */
			fb_data->update_marker_array[j].update_marker = 0;
		}
	}

	/* Check to see if all updates have completed */
	if (is_free_list_full(fb_data) &&
		(fb_data->cur_update == NULL) &&
		!epdc_any_luts_active()) {

		if (fb_data->pwrdown_delay != FB_POWERDOWN_DISABLE) {
			/*
			 * Set variable to prevent overlapping
			 * enable/disable requests
			 */
			fb_data->powering_down = true;

			/* Schedule task to disable EPDC HW until next update */
			schedule_delayed_work(&fb_data->epdc_done_work,
				msecs_to_jiffies(fb_data->pwrdown_delay));

			/* Reset counter to reduce chance of overflow */
			fb_data->order_cnt = 0;
		}

		if (fb_data->waiting_for_idle)
			complete(&fb_data->updates_done);
	}

	/* Is Working Buffer busy? */
	if (epdc_is_working_buffer_busy()) {
		/* Can't submit another update until WB is done */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return IRQ_HANDLED;
	}

	/*
	 * Were we waiting on working buffer?
	 * If so, update queues and check for collisions
	 */
	if (fb_data->cur_update != NULL) {
		dev_dbg(fb_data->dev, "\nWorking buffer completed\n");

		/* Signal completion if submit workqueue was waiting on WB */
		if (fb_data->waiting_for_wb) {
			complete(&fb_data->update_res_free);
			fb_data->waiting_for_lut = false;
		}

		/*
		 * Check for "missed collision" conditions:
		 *  - Current update overlaps one or more updates
		 *    in collision list
		 *  - No collision reported with current active updates
		 */
		list_for_each_entry(collision_update,
				    &fb_data->upd_buf_collision_list->list,
				    list)
			if (do_updates_overlap(collision_update,
				fb_data->cur_update))
				missed_coll_mask |=
					collision_update->collision_mask;

		/* Was there a collision? */
		if (epdc_is_collision() || missed_coll_mask) {
			/* Check list of colliding LUTs, and add to our collision mask */
			fb_data->cur_update->collision_mask =
			    epdc_get_colliding_luts();

			if (!fb_data->cur_update->collision_mask) {
				fb_data->cur_update->collision_mask =
					missed_coll_mask;
				dev_dbg(fb_data->dev, "Missed collision "
					"possible. Mask = 0x%x\n",
					missed_coll_mask);
			}

			/* Clear collisions that completed since WB began */
			fb_data->cur_update->collision_mask &=
				~fb_data->luts_complete_wb;

			dev_dbg(fb_data->dev, "\nCollision mask = 0x%x\n",
			       fb_data->cur_update->collision_mask);

			/*
			 * If we collide with newer updates, then
			 * we don't need to re-submit the update. The
			 * idea is that the newer updates should take
			 * precedence anyways, so we don't want to
			 * overwrite them.
			 */
			for (temp_mask = fb_data->cur_update->collision_mask, lut = 0;
				temp_mask != 0;
				lut++, temp_mask = temp_mask >> 1) {
				if (!(temp_mask & 0x1))
					continue;

				if (fb_data->lut_update_order[lut] >=
					fb_data->cur_update->update_order) {
					dev_dbg(fb_data->dev, "Ignoring collision with newer update.\n");
					ignore_collision = true;
					break;
				}
			}

			if (ignore_collision) {
				/* Add to free buffer list */
				list_add_tail(&fb_data->cur_update->list,
					 &fb_data->upd_buf_free_list->list);
/* 2011/04/19 FY11 : Supported wake lock. */
				epdc_wake_unlock_for_update_buffer( fb_data );
			} else {
				/*
				 * If update has a marker, clear the LUT, since we
				 * don't want to signal that it is complete.
				 */
				if (fb_data->cur_update->upd_marker_data)
					if (fb_data->cur_update->upd_marker_data->update_marker != 0)
						fb_data->cur_update->upd_marker_data->lut_num = INVALID_LUT;

				/* Move to collision list */
				list_add_tail(&fb_data->cur_update->list,
					 &fb_data->upd_buf_collision_list->list);
			}
		} else {
			/* Add to free buffer list */
			list_add_tail(&fb_data->cur_update->list,
				 &fb_data->upd_buf_free_list->list);
/* 2011/04/19 FY11 : Supported wake lock. */
			epdc_wake_unlock_for_update_buffer( fb_data );
		}
		/* Clear current update */
		fb_data->cur_update = NULL;

		/* Clear IRQ for working buffer */
		epdc_working_buf_intr(false);
		epdc_clear_working_buf_irq();
	}

	if (fb_data->upd_scheme != UPDATE_SCHEME_SNAPSHOT) {
		/* Queued update scheme processing */

		/* Schedule task to submit collision and pending update */
		if (!fb_data->powering_down)
			queue_work(fb_data->epdc_submit_workqueue,
				&fb_data->epdc_submit_work);

		/* Release buffer queues */
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);

		return IRQ_HANDLED;
	}

	/* Snapshot update scheme processing */

	/* Check to see if any LUTs are free */
	if (!epdc_any_luts_available()) {
		dev_dbg(fb_data->dev, "No luts available.\n");
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return IRQ_HANDLED;
	}

	/* Check to see if there is a valid LUT to use */
	ret = epdc_choose_next_lut(&next_lut);
	if (ret && fb_data->tce_prevent) {
		dev_dbg(fb_data->dev, "Must wait for LUT15\n");
		spin_unlock_irqrestore(&fb_data->queue_lock, flags);
		return IRQ_HANDLED;
	}

	/*
	 * Are any of our collision updates able to go now?
	 * Go through all updates in the collision list and check to see
	 * if the collision mask has been fully cleared
	 */
	list_for_each_entry(collision_update,
			    &fb_data->upd_buf_collision_list->list, list) {

		if (collision_update->collision_mask != 0)
			continue;

		dev_dbg(fb_data->dev, "A collision update is ready to go!\n");
		/*
		 * We have a collision cleared, so select it
		 * and we will retry the update
		 */
		fb_data->cur_update = collision_update;
		list_del_init(&fb_data->cur_update->list);
		break;
	}

	/*
	 * If we didn't find a collision update ready to go,
	 * we try to grab one from the update queue
	 */
	if (fb_data->cur_update == NULL) {
		/* Is update list empty? */
		if (list_empty(&fb_data->upd_buf_queue->list)) {
			dev_dbg(fb_data->dev, "No pending updates.\n");

			/* No updates pending, so we are done */
			spin_unlock_irqrestore(&fb_data->queue_lock, flags);
			return IRQ_HANDLED;
		} else {
			dev_dbg(fb_data->dev, "Found a pending update!\n");

			/* Process next item in update list */
			fb_data->cur_update =
			    list_entry(fb_data->upd_buf_queue->list.next,
				       struct update_data_list, list);
			list_del_init(&fb_data->cur_update->list);
		}
	}

	/* Use LUT selected above */
	fb_data->cur_update->lut_num = next_lut;

	/* Associate LUT with update marker */
	if ((fb_data->cur_update->upd_marker_data)
		&& (fb_data->cur_update->upd_marker_data->update_marker != 0))
		fb_data->cur_update->upd_marker_data->lut_num =
						fb_data->cur_update->lut_num;

	/* Mark LUT as containing new update */
	fb_data->lut_update_order[fb_data->cur_update->lut_num] =
		fb_data->cur_update->update_order;

	/* Enable Collision and WB complete IRQs */
	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(fb_data->cur_update->lut_num, true);

	/* Program EPDC update to process buffer */
	next_upd_region = &fb_data->cur_update->upd_data.update_region;
	if (fb_data->cur_update->upd_data.temp != TEMP_USE_AMBIENT) {
		temp_index = mxc_epdc_fb_get_temp_index(fb_data, fb_data->cur_update->upd_data.temp);
		epdc_set_temp(temp_index);
	} else
		epdc_set_temp(fb_data->temp_index);
	epdc_set_update_addr(fb_data->cur_update->phys_addr + fb_data->cur_update->epdc_offs);
	epdc_set_update_coord(next_upd_region->left, next_upd_region->top);
	epdc_set_update_dimensions(next_upd_region->width,
				   next_upd_region->height);

	epdc_submit_update(fb_data->cur_update->lut_num,
			   fb_data->cur_update->upd_data.waveform_mode,
			   fb_data->cur_update->upd_data.update_mode, false, 0);

	/* Release buffer queues */
	spin_unlock_irqrestore(&fb_data->queue_lock, flags);

	return IRQ_HANDLED;
}

/* 2011/04/06 FY11 : Modified to power on/off in this method. */
static void draw_mode0(struct mxc_epdc_fb_data *fb_data)
{
	u32 *upd_buf_ptr;
	int i, ret;
	struct fb_var_screeninfo *screeninfo = &fb_data->epdc_fb_var;
	u32 xres, yres;

/* 2011/04/06 FY11 : Added power on. */
	ret = epdc_powerup(fb_data);
	if ( ret < 0 )
	{
		printk( KERN_ERR "%s Fails to powerup.%d\n", __func__, ret );
		return;
	}

	upd_buf_ptr = (u32 *)fb_data->info.screen_base;

	epdc_working_buf_intr(true);
	epdc_lut_complete_intr(0, true);
	fb_data->in_init = true;

	/* Use unrotated (native) width/height */
	if ((screeninfo->rotate == FB_ROTATE_CW) ||
		(screeninfo->rotate == FB_ROTATE_CCW)) {
		xres = screeninfo->yres;
		yres = screeninfo->xres;
	} else {
		xres = screeninfo->xres;
		yres = screeninfo->yres;
	}

	/* Program EPDC update to process buffer */
	epdc_set_update_addr(fb_data->phys_start);
	epdc_set_update_coord(0, 0);
	epdc_set_update_dimensions(xres, yres);
/* 2011/03/08 FY11 : Added reading temperature. */
	if ( mxc_epdc_fb_read_temperature( fb_data ) )
	{
		printk( KERN_ERR "%s fails to init panel.\n", __func__ );
	}
	epdc_set_temp(fb_data->temp_index);
	epdc_submit_update(0, fb_data->wv_modes.mode_init, UPDATE_MODE_FULL, true, 0xFF);

	dev_dbg(fb_data->dev, "Mode0 update - Waiting for LUT to complete...\n");

	/* Will timeout after ~4-5 seconds */

	for (i = 0; i < 40; i++) {
		if (!epdc_is_lut_active(0)) {
			dev_dbg(fb_data->dev, "Mode0 init complete\n");
/* 2011/04/06 FY11 : Added power off. */
/* 2011/06/09 FY11 : Fixed the failure of power off. */
			fb_data->powering_down = true;
			schedule_delayed_work(&fb_data->epdc_done_work,
						msecs_to_jiffies(fb_data->pwrdown_delay));
			return;
		}
		msleep(100);
	}

/* 2011/04/06 FY11 : Added power off. */
/* 2011/06/09 FY11 : Fixed the failure of power off. */
	fb_data->powering_down = true;
	schedule_delayed_work(&fb_data->epdc_done_work,
				msecs_to_jiffies(fb_data->pwrdown_delay));
	dev_err(fb_data->dev, "Mode0 init failed!\n");

	return;
}


/*2011/2/17 FY11 : Supported to read waveform in eMMC. */
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
static int mxc_epdc_fb_load_wf( struct mxc_epdc_fb_data *fb_data, bool from_eMMC )
{
	int ret = 0;
	struct mxcfb_waveform_data_file *wv_file;
	int wv_data_offs;
	int i;
/* 2011/03/08 FY11 : Removed panel update just after init. */

	unsigned long ulWfIndex, ulWfSize;
	u8 *pucWfVer = NULL;


/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
	struct resource * res_waveform;
	ulWfIndex = rawdata_index( "Waveform", &ulWfSize );
#ifdef CONFIG_EPD_STATIC_MEM_WAVEFORM
	res_waveform = get_res_epd_waveform();
	ulWfSize = (unsigned long)(res_waveform->end - res_waveform->start +1);
#endif

	if ( fb_data->waveform_buffer_virt == NULL )
	{
#ifdef CONFIG_EPD_STATIC_MEM_WAVEFORM
		fb_data->waveform_buffer_virt = (u32 *)ioremap((phys_addr_t)res_waveform->start, ulWfSize );
		fb_data->waveform_buffer_phys = res_waveform->start;
#else
		/* Allocate memory for waveform data */
		fb_data->waveform_buffer_virt = dma_alloc_coherent(fb_data->dev,
							fb_data->waveform_buffer_size,
							&fb_data->waveform_buffer_phys,
							GFP_DMA);
#endif
		if (fb_data->waveform_buffer_virt == NULL) 
		{
			dev_err(fb_data->dev, "Can't allocate mem for waveform!\n");
			return -ENOMEM;
		}
	}


	if ( from_eMMC )
	{
/* 2011/06/08 FY11 : Modified to use static buffer for waveform read. */
		u8 *pCurBuf = NULL;

		/* read wf header */
		ret = rawdata_read( ulWfIndex,
				    0,
				    wf_tmp_buf,
				    WF_TMP_BUF_SIZE );
		if ( ret < 0 )
		{
			printk( KERN_ERR "%s fail to read wf header.\n", __func__ );
			return ret;
		}

		/* get waveform header.  (wf_tmp_buf must be larger than waveform header. */
		wv_file = (struct mxcfb_waveform_data_file *)wf_tmp_buf;

		/* Get size and allocate temperature range table */
		fb_data->trt_entries = wv_file->wdh.trc + 1;

/* 2011/03/08 FY11 : Supported to write waveform and reload. */
		/* free temp range table if it has already exist. */
		if ( fb_data->temp_range_bounds )
		{
			kzfree( fb_data->temp_range_bounds );
			fb_data->temp_range_bounds = NULL;
		}
		fb_data->temp_range_bounds = kzalloc(fb_data->trt_entries, GFP_KERNEL);
		if ( fb_data->temp_range_bounds == NULL )
		{
			printk(KERN_ERR "%s fail to mallock trt table.\n", __func__ );
			return -ENOMEM;
		}

		/* Copy TRT data. (wf_tmp_buf must be larger than waveform header + 256 bytes) */
		memcpy(fb_data->temp_range_bounds, &wv_file->data, fb_data->trt_entries);


		/* Get offset and size for waveform data */
		wv_data_offs = sizeof(wv_file->wdh) + fb_data->trt_entries + 1;
		fb_data->waveform_buffer_size = ulWfSize - wv_data_offs;


		/* set the pointer to waveform version. */
		pucWfVer = wf_tmp_buf + WF_VER_OFFSET;
/* 2011/2/24 FY11 : Supported to read waveform version. */
		memcpy( &(s_st_wfversion.version[0]), (u8*)pucWfVer, WF_VER_LEN );


		// copy waveform data from temporary buffer.
		pCurBuf = (u8*)(fb_data->waveform_buffer_virt);
		memcpy( pCurBuf,
			wf_tmp_buf + wv_data_offs,
			WF_TMP_BUF_SIZE - wv_data_offs );
		pCurBuf += (WF_TMP_BUF_SIZE - wv_data_offs);

		for ( i = WF_TMP_BUF_SIZE; i < ulWfSize; i+=WF_TMP_BUF_SIZE )
		{
			ret = rawdata_read( ulWfIndex,
					    i,
					    wf_tmp_buf,
					    WF_TMP_BUF_SIZE );
			if ( ret < 0 )
			{
				printk( KERN_ERR "%s fail to read wf.\n", __func__ );
				return ret;
			}

			memcpy( pCurBuf, wf_tmp_buf, ( ret < WF_TMP_BUF_SIZE ? ret : WF_TMP_BUF_SIZE ) );
			pCurBuf += WF_TMP_BUF_SIZE;
		}
		ret = 0;

	}
#ifdef CONFIG_EPD_STATIC_MEM_WAVEFORM
	else	// use data on RAM
	{
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
		u8 *ptmp = NULL;
		unsigned long res_wf_size = ulWfSize;

		fb_data->waveform_buffer_size = res_wf_size - sizeof(struct epd_settings) - sizeof(unsigned long);

		fb_data->trt_entries = *((unsigned long*)((u8*)(fb_data->waveform_buffer_virt) + fb_data->waveform_buffer_size));

		fb_data->temp_range_bounds = kzalloc(fb_data->trt_entries, GFP_KERNEL);
		if ( fb_data->temp_range_bounds == NULL )
		{
			printk(KERN_ERR "%s fail to mallock trt table.\n", __func__ );
			return -ENOMEM;
		}
		fb_data->waveform_buffer_size -= fb_data->trt_entries;
		ptmp = (u8*)(fb_data->waveform_buffer_virt) + fb_data->waveform_buffer_size;
		/* Copy TRT data */
		memcpy(fb_data->temp_range_bounds, ptmp, fb_data->trt_entries);

		fb_data->waveform_buffer_size -= WF_VER_LEN;
		pucWfVer = (u8*)(fb_data->waveform_buffer_virt) + fb_data->waveform_buffer_size;
/* 2011/2/24 FY11 : Supported to read waveform version. */
		memcpy( &(s_st_wfversion.version[0]), (u8*)pucWfVer, WF_VER_LEN );
	}
#endif

	/* Set default temperature index using TRT and room temp */
	fb_data->temp_index = mxc_epdc_fb_get_temp_index(fb_data, DEFAULT_TEMP);

	/* Enable clocks to access EPDC regs */
	clk_enable(fb_data->epdc_clk_axi);

	/* Enable pix clk for EPDC */
	clk_enable(fb_data->epdc_clk_pix);
	clk_set_rate(fb_data->epdc_clk_pix, fb_data->cur_mode->vmode->pixclock);

	epdc_init_sequence(fb_data);

	/* Disable clocks */
	clk_disable(fb_data->epdc_clk_axi);
	clk_disable(fb_data->epdc_clk_pix);

	fb_data->hw_ready = true;

/* 2011/03/08 FY11 : Removed panel update just after init. */


	return ret;
}


static int mxc_epdc_fb_init_hw(struct fb_info *info)
{
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;
	int ret;
	char *p = NULL;
	bool read_from_eMMC = true;

	/*
	 * Create fw search string based on ID string in selected videomode.
	 * Format is "imx/epdc_[panel string].fw"
	 */
	if (fb_data->cur_mode) {
		strcat(fb_data->fw_str, "imx/epdc_");
		strcat(fb_data->fw_str, fb_data->cur_mode->vmode->name);
		strcat(fb_data->fw_str, ".fw");
	}

	fb_data->fw_default_load = false;

/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
	p = strstr(saved_command_line, "nfsroot=" );
	if ( p == NULL )
	{
		// if not nfs boot, read wf from eMMC.
		read_from_eMMC = false;
	}

/*2011/2/17 FY11 : Supported to read waveform in eMMC. */
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
	ret = mxc_epdc_fb_load_wf( fb_data , read_from_eMMC );
	if (ret)
		dev_err(fb_data->dev,
			"Failed request_firmware_nowait err %d\n", ret);

/*2011/04/06 FY11 : Added panel init for NFS boot. */
	if ( p )
	{
		draw_mode0(fb_data);
	}

	return ret;
}

static ssize_t store_update(struct device *device,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mxcfb_update_data update;
	struct fb_info *info = dev_get_drvdata(device);
	struct mxc_epdc_fb_data *fb_data = (struct mxc_epdc_fb_data *)info;

	if (strncmp(buf, "direct", 6) == 0)
		update.waveform_mode = fb_data->wv_modes.mode_du;
	else if (strncmp(buf, "gc16", 4) == 0)
		update.waveform_mode = fb_data->wv_modes.mode_gc16;
	else if (strncmp(buf, "gc4", 3) == 0)
		update.waveform_mode = fb_data->wv_modes.mode_gc4;

	/* Now, request full screen update */
	update.update_region.left = 0;
	update.update_region.width = fb_data->epdc_fb_var.xres;
	update.update_region.top = 0;
	update.update_region.height = fb_data->epdc_fb_var.yres;
	update.update_mode = UPDATE_MODE_FULL;
	update.temp = TEMP_USE_AMBIENT;
	update.update_marker = 0;
	update.flags = 0;

	mxc_epdc_fb_send_update(&update, info, false);

	return count;
}

static struct device_attribute fb_attrs[] = {
	__ATTR(update, S_IRUGO|S_IWUSR, NULL, store_update),
};

int __devinit mxc_epdc_fb_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct mxc_epdc_fb_data *fb_data;
	struct resource *res;
	struct fb_info *info;
	char *options, *opt;
	char *panel_str = NULL;
	char name[] = "mxcepdcfb";
	struct fb_videomode *vmode;
	int xres_virt, yres_virt, buf_size;
	int xres_virt_rot, yres_virt_rot, pix_size_rot;
	struct fb_var_screeninfo *var_info;
	struct fb_fix_screeninfo *fix_info;
	struct pxp_config_data *pxp_conf;
	struct pxp_proc_data *proc_data;
	struct scatterlist *sg;
	struct update_data_list *upd_list;
	struct update_data_list *plist, *temp_list;
	int i;
	unsigned long x_mem_size = 0;
/* 2011/03/05 FY11 : Supported A2 mode limitations. */
	unsigned short *pusWhiteBuf = NULL;


	fb_data = (struct mxc_epdc_fb_data *)framebuffer_alloc(
			sizeof(struct mxc_epdc_fb_data), &pdev->dev);
	if (fb_data == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	/* Get platform data and check validity */
	fb_data->pdata = pdev->dev.platform_data;
	if ((fb_data->pdata == NULL) || (fb_data->pdata->num_modes < 1)
		|| (fb_data->pdata->epdc_mode == NULL)
		|| (fb_data->pdata->epdc_mode->vmode == NULL)) {
		ret = -EINVAL;
		goto out_fbdata;
	}

	if (fb_get_options(name, &options)) {
		ret = -ENODEV;
		goto out_fbdata;
	}

	fb_data->tce_prevent = 0;

	if (options)
		while ((opt = strsep(&options, ",")) != NULL) {
			if (!*opt)
				continue;

			if (!strncmp(opt, "bpp=", 4))
				fb_data->default_bpp =
					simple_strtoul(opt + 4, NULL, 0);
			else if (!strncmp(opt, "x_mem=", 6))
				x_mem_size = memparse(opt + 6, NULL);
			else if (!strncmp(opt, "tce_prevent", 4))
				fb_data->tce_prevent = 1;
			else
				panel_str = opt;
		}

	fb_data->dev = &pdev->dev;

	if (!fb_data->default_bpp)
		fb_data->default_bpp = 16;

	/* Set default (first defined mode) before searching for a match */
	fb_data->cur_mode = &fb_data->pdata->epdc_mode[0];

	if (panel_str)
		for (i = 0; i < fb_data->pdata->num_modes; i++)
			if (!strcmp(fb_data->pdata->epdc_mode[i].vmode->name,
						panel_str)) {
				fb_data->cur_mode =
					&fb_data->pdata->epdc_mode[i];
				break;
			}

	vmode = fb_data->cur_mode->vmode;

	platform_set_drvdata(pdev, fb_data);
	info = &fb_data->info;

	/* Allocate color map for the FB */
	ret = fb_alloc_cmap(&info->cmap, 256, 0);
	if (ret)
		goto out_fbdata;

	dev_dbg(&pdev->dev, "resolution %dx%d, bpp %d\n",
		vmode->xres, vmode->yres, fb_data->default_bpp);

	/*
	 * GPU alignment restrictions dictate framebuffer parameters:
	 * - 32-byte alignment for buffer width
	 * - 128-byte alignment for buffer height
	 * => 4K buffer alignment for buffer start
	 */
	xres_virt = ALIGN(vmode->xres, 32);
	yres_virt = ALIGN(vmode->yres, 128);
	fb_data->max_pix_size = PAGE_ALIGN(xres_virt * yres_virt);

	/*
	 * Have to check to see if aligned buffer size when rotated
	 * is bigger than when not rotated, and use the max
	 */
	xres_virt_rot = ALIGN(vmode->yres, 32);
	yres_virt_rot = ALIGN(vmode->xres, 128);
	pix_size_rot = PAGE_ALIGN(xres_virt_rot * yres_virt_rot);
	fb_data->max_pix_size = (fb_data->max_pix_size > pix_size_rot) ?
				fb_data->max_pix_size : pix_size_rot;

	buf_size = fb_data->max_pix_size * fb_data->default_bpp/8;

	/* Compute the number of screens needed based on X memory requested */
	if (x_mem_size > 0) {
		fb_data->num_screens = DIV_ROUND_UP(x_mem_size, buf_size);
		if (fb_data->num_screens < NUM_SCREENS_MIN)
			fb_data->num_screens = NUM_SCREENS_MIN;
		else if (buf_size * fb_data->num_screens > SZ_16M)
			fb_data->num_screens = SZ_16M / buf_size;
	} else
		fb_data->num_screens = NUM_SCREENS_MIN;

	fb_data->map_size = buf_size * fb_data->num_screens;
	dev_dbg(&pdev->dev, "memory to allocate: %d\n", fb_data->map_size);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		ret = -ENODEV;
		goto out_cmap;
	}

	epdc_base = ioremap(res->start, SZ_4K);
	if (epdc_base == NULL) {
		ret = -ENOMEM;
		goto out_cmap;
	}

	/* Allocate FB memory */
	info->screen_base = dma_alloc_writecombine(&pdev->dev,
						  fb_data->map_size + buf_size, 
							/* 2011/03/15 FY11 : Supported standby screen. */
						  &fb_data->phys_start,
						  GFP_DMA);

	if (info->screen_base == NULL) {
		ret = -ENOMEM;
		goto out_mapregs;
	}
	dev_dbg(&pdev->dev, "allocated at %p:0x%x\n", info->screen_base,
		fb_data->phys_start);

/* 2011/03/05 FY11 : Supported A2 mode limitations. */
	fb_data->force_wf_mode = false;

/* 2011/03/15 FY11 : Supported standby screen. */
	fb_data->standbyscreen_buff_size = buf_size;
	fb_data->standbyscreen_buff_virt = (info->screen_base) + fb_data->map_size;
	fb_data->standbyscreen_buff = (fb_data->phys_start) + fb_data->map_size;
	/* Initialize standby screen buffer */
	pusWhiteBuf = (unsigned short*)(fb_data->standbyscreen_buff_virt);
	for ( i = 0; i < (buf_size)/sizeof(unsigned short); i++ )
	{
		pusWhiteBuf[i] = 0x630C;
	}
	

	var_info = &info->var;
	var_info->activate = FB_ACTIVATE_TEST;
	var_info->bits_per_pixel = fb_data->default_bpp;
	var_info->xres = vmode->xres;
	var_info->yres = vmode->yres;
	var_info->xres_virtual = xres_virt;
	/* Additional screens allow for panning  and buffer flipping */
	var_info->yres_virtual = yres_virt * fb_data->num_screens;

	var_info->pixclock = vmode->pixclock;
	var_info->left_margin = vmode->left_margin;
	var_info->right_margin = vmode->right_margin;
	var_info->upper_margin = vmode->upper_margin;
	var_info->lower_margin = vmode->lower_margin;
	var_info->hsync_len = vmode->hsync_len;
	var_info->vsync_len = vmode->vsync_len;
	var_info->vmode = FB_VMODE_NONINTERLACED;

	switch (fb_data->default_bpp) {
	case 32:
	case 24:
		var_info->red.offset = 16;
		var_info->red.length = 8;
		var_info->green.offset = 8;
		var_info->green.length = 8;
		var_info->blue.offset = 0;
		var_info->blue.length = 8;
		break;

	case 16:
		var_info->red.offset = 11;
		var_info->red.length = 5;
		var_info->green.offset = 5;
		var_info->green.length = 6;
		var_info->blue.offset = 0;
		var_info->blue.length = 5;
		break;

	case 8:
		/*
		 * For 8-bit grayscale, R, G, and B offset are equal.
		 *
		 */
		var_info->grayscale = GRAYSCALE_8BIT;

		var_info->red.length = 8;
		var_info->red.offset = 0;
		var_info->red.msb_right = 0;
		var_info->green.length = 8;
		var_info->green.offset = 0;
		var_info->green.msb_right = 0;
		var_info->blue.length = 8;
		var_info->blue.offset = 0;
		var_info->blue.msb_right = 0;
		break;

	default:
		dev_err(&pdev->dev, "unsupported bitwidth %d\n",
			fb_data->default_bpp);
		ret = -EINVAL;
		goto out_dma_fb;
	}

	fix_info = &info->fix;

	strcpy(fix_info->id, "mxc_epdc_fb");
	fix_info->type = FB_TYPE_PACKED_PIXELS;
	fix_info->visual = FB_VISUAL_TRUECOLOR;
	fix_info->xpanstep = 0;
	fix_info->ypanstep = 0;
	fix_info->ywrapstep = 0;
	fix_info->accel = FB_ACCEL_NONE;
	fix_info->smem_start = fb_data->phys_start;
	fix_info->smem_len = fb_data->map_size;
	fix_info->ypanstep = 0;

	fb_data->native_width = vmode->xres;
	fb_data->native_height = vmode->yres;

	info->fbops = &mxc_epdc_fb_ops;
	info->var.activate = FB_ACTIVATE_NOW;
	info->pseudo_palette = fb_data->pseudo_palette;
	info->screen_size = info->fix.smem_len;
	info->flags = FBINFO_FLAG_DEFAULT;

	mxc_epdc_fb_set_fix(info);

	fb_data->auto_mode = AUTO_UPDATE_MODE_REGION_MODE;
	fb_data->upd_scheme = UPDATE_SCHEME_QUEUE_AND_MERGE; // UPDATE_SCHEME_SNAPSHOT;

	/* Initialize our internal copy of the screeninfo */
	fb_data->epdc_fb_var = *var_info;
	fb_data->fb_offset = 0;
	fb_data->eof_sync_period = 0;

	/* Allocate head objects for our lists */
	fb_data->upd_buf_queue =
	    kzalloc(sizeof(struct update_data_list), GFP_KERNEL);
	fb_data->upd_buf_collision_list =
	    kzalloc(sizeof(struct update_data_list), GFP_KERNEL);
	fb_data->upd_buf_free_list =
	    kzalloc(sizeof(struct update_data_list), GFP_KERNEL);
	if ((fb_data->upd_buf_queue == NULL) || (fb_data->upd_buf_free_list == NULL)
	    || (fb_data->upd_buf_collision_list == NULL)) {
		ret = -ENOMEM;
		goto out_dma_fb;
	}

	/*
	 * Initialize lists for update requests, update collisions,
	 * and available update (PxP output) buffers
	 */
	INIT_LIST_HEAD(&fb_data->upd_buf_queue->list);
	INIT_LIST_HEAD(&fb_data->upd_buf_free_list->list);
	INIT_LIST_HEAD(&fb_data->upd_buf_collision_list->list);

	/* Allocate update buffers and add them to the list */
	for (i = 0; i < EPDC_MAX_NUM_UPDATES; i++) {
		upd_list = kzalloc(sizeof(*upd_list), GFP_KERNEL);
		if (upd_list == NULL) {
			ret = -ENOMEM;
			goto out_upd_buffers;
		}

		/* Clear update data structure */
		memset(&upd_list->upd_data, 0,
		       sizeof(struct mxcfb_update_data));

		/*
		 * Allocate memory for PxP output buffer.
		 * Each update buffer is 1 byte per pixel, and can
		 * be as big as the full-screen frame buffer
		 */
		upd_list->virt_addr =
		    dma_alloc_coherent(fb_data->info.device, fb_data->max_pix_size,
				       &upd_list->phys_addr, GFP_DMA);
		if (upd_list->virt_addr == NULL) {
			kfree(upd_list);
			ret = -ENOMEM;
			goto out_upd_buffers;
		}

		/* Add newly allocated buffer to free list */
		list_add(&upd_list->list, &fb_data->upd_buf_free_list->list);

		dev_dbg(fb_data->info.device, "allocated %d bytes @ 0x%08X\n",
			fb_data->max_pix_size, upd_list->phys_addr);
	}

	/*
	 * Allocate memory for PxP SW workaround buffer
	 * These buffers are used to hold copy of the update region,
	 * before sending it to PxP for processing.
	 */
	fb_data->virt_addr_copybuf =
	    dma_alloc_coherent(fb_data->info.device, fb_data->max_pix_size*2,
			       &fb_data->phys_addr_copybuf, GFP_DMA);
	if (fb_data->virt_addr_copybuf == NULL) {
		ret = -ENOMEM;
		goto out_upd_buffers;
	}

	fb_data->working_buffer_size = vmode->yres * vmode->xres * 2;
	/* Allocate memory for EPDC working buffer */
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
#ifdef CONFIG_EPD_STATIC_MEM_WORKBUFF
	{
	struct resource * res_wb = get_res_epd_workbuff();
	unsigned long res_wb_size = (unsigned long)(res_wb->end - res_wb->start +1);
	fb_data->working_buffer_virt = (u32 *)ioremap((phys_addr_t)res_wb->start, res_wb_size );
	fb_data->working_buffer_phys = res_wb->start;
	}
#else
	fb_data->working_buffer_virt =
	    dma_alloc_coherent(&pdev->dev, fb_data->working_buffer_size,
			       &fb_data->working_buffer_phys, GFP_DMA);
#endif
	if (fb_data->working_buffer_virt == NULL) {
		dev_err(&pdev->dev, "Can't allocate mem for working buf!\n");
		ret = -ENOMEM;
		goto out_copybuffer;
	}

/* 2011/05/12 FY11 : Initialize working buffer. */
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
#ifdef CONFIG_EPD_STATIC_MEM_WORKBUFF
#else
	memset( fb_data->working_buffer_virt, 0xFF, fb_data->working_buffer_size );
#endif


/* 2011/06/08 FY11 : Modified to use static buffer for waveofrm. */
	wf_tmp_buf = kmalloc( WF_TMP_BUF_SIZE, GFP_KERNEL );
	if( wf_tmp_buf == NULL )
	{
		ret = -ENOMEM;
		goto out_dma_work_buf;
	}


	/* Initialize EPDC pins */
	if (fb_data->pdata->get_pins)
		fb_data->pdata->get_pins();

	fb_data->epdc_clk_axi = clk_get(fb_data->dev, "epdc_axi");
	if (IS_ERR(fb_data->epdc_clk_axi)) {
		dev_err(&pdev->dev, "Unable to get EPDC AXI clk."
			"err = 0x%x\n", (int)fb_data->epdc_clk_axi);
		ret = -ENODEV;
		goto out_wf_tmp_buf;
	}
	fb_data->epdc_clk_pix = clk_get(fb_data->dev, "epdc_pix");
	if (IS_ERR(fb_data->epdc_clk_pix)) {
		dev_err(&pdev->dev, "Unable to get EPDC pix clk."
			"err = 0x%x\n", (int)fb_data->epdc_clk_pix);
		ret = -ENODEV;
		goto out_wf_tmp_buf;
	}

	fb_data->in_init = false;

	fb_data->hw_ready = false;

	/*
	 * Set default waveform mode values.
	 * Should be overwritten via ioctl.
	 */
	fb_data->wv_modes.mode_init = 0;
	fb_data->wv_modes.mode_du = 1;
	fb_data->wv_modes.mode_gc4 = 3;
	fb_data->wv_modes.mode_gc8 = 2;
	fb_data->wv_modes.mode_gc16 = 2;
	fb_data->wv_modes.mode_gc32 = 2;
/* 2011/03/05 FY11 : Supported A2 mode limitations. */
	fb_data->wv_modes.mode_a2 = 4;

	/* Initialize markers */
	for (i = 0; i < EPDC_MAX_NUM_UPDATES; i++) {
		fb_data->update_marker_array[i].update_marker = 0;
		fb_data->update_marker_array[i].lut_num = INVALID_LUT;
	}

	/* Initialize all LUTs to inactive */
	for (i = 0; i < EPDC_NUM_LUTS; i++)
		fb_data->lut_update_order[i] = 0;

	/* Retrieve EPDC IRQ num */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		dev_err(&pdev->dev, "cannot get IRQ resource\n");
		ret = -ENODEV;
		goto out_wf_tmp_buf;
	}
	fb_data->epdc_irq = res->start;

	/* Register IRQ handler */
	ret = request_irq(fb_data->epdc_irq, mxc_epdc_irq_handler, 0,
			"fb_dma", fb_data);
	if (ret) {
		dev_err(&pdev->dev, "request_irq (%d) failed with error %d\n",
			fb_data->epdc_irq, ret);
		ret = -ENODEV;
		goto out_wf_tmp_buf;
	}

	INIT_DELAYED_WORK(&fb_data->epdc_done_work, epdc_done_work_func);
	fb_data->epdc_submit_workqueue = create_rt_workqueue("submit");
	INIT_WORK(&fb_data->epdc_submit_work, epdc_submit_work_func);

	info->fbdefio = &mxc_epdc_fb_defio;
#ifdef CONFIG_FB_MXC_EINK_AUTO_UPDATE_MODE
	fb_deferred_io_init(info);
#endif

	/* get pmic regulators */
/* 2011/07/05 FY11 : Added VSYS_EPD_ON. */
	fb_data->vsys_regulator = regulator_get(NULL, "VSYS_EPD");
	if (IS_ERR(fb_data->vsys_regulator)) {
		dev_err(&pdev->dev, "Unable to get vsys PMIC regulator."
			"err = 0x%x\n", (int)fb_data->vsys_regulator);
		ret = -ENODEV;
		goto out_irq;
	}
	regulator_enable(fb_data->vsys_regulator);
	fb_data->display_regulator = regulator_get(NULL, "DISPLAY");
	if (IS_ERR(fb_data->display_regulator)) {
		dev_err(&pdev->dev, "Unable to get display PMIC regulator."
			"err = 0x%x\n", (int)fb_data->display_regulator);
		ret = -ENODEV;
		goto out_irq;
	}
	fb_data->vcom_regulator = regulator_get(NULL, "VCOM");
	if (IS_ERR(fb_data->vcom_regulator)) {
		regulator_put(fb_data->display_regulator);
		dev_err(&pdev->dev, "Unable to get VCOM regulator."
			"err = 0x%x\n", (int)fb_data->vcom_regulator);
		ret = -ENODEV;
		goto out_irq;
	}
	fb_data->epd_pwr0_regulator = regulator_get(NULL, "PWR0_CTRL");
	if (IS_ERR(fb_data->epd_pwr0_regulator)) {
		dev_err(&pdev->dev, "Unable to get PWR0_CTRL regulator."
			"err = 0x%x\n", (int)fb_data->epd_pwr0_regulator);
		ret = -ENODEV;
		goto out_irq;
	}

	fb_data->epd_pwr2_regulator = regulator_get(NULL, "PWR2_CTRL");
	if (IS_ERR(fb_data->epd_pwr2_regulator)) {
		dev_err(&pdev->dev, "Unable to get PWR2_CTRL regulator."
			"err = 0x%x\n", (int)fb_data->epd_pwr2_regulator);
		ret = -ENODEV;
		goto out_irq;
	}

	fb_data->temp_regulator = regulator_get(NULL, "PMIC_TEMP");
	if (IS_ERR(fb_data->temp_regulator)) {
		dev_err(&pdev->dev, "Unable to get PMIC TEMP regulator."
			"err = 0x%x\n", (int)fb_data->temp_regulator);
		ret = -ENODEV;
		goto out_irq;
	}


	fb_data->v3p3_regulator = regulator_get(NULL, "V3P3_CTRL");
	if (IS_ERR(fb_data->v3p3_regulator)) {
		dev_err(&pdev->dev, "Unable to get PMIC 3.3V regulator."
			"err = 0x%x\n", (int)fb_data->v3p3_regulator);
		ret = -ENODEV;
		goto out_irq;
	}


	if (device_create_file(info->dev, &fb_attrs[0]))
		dev_err(&pdev->dev, "Unable to create file from fb_attrs\n");

	fb_data->cur_update = NULL;

	spin_lock_init(&fb_data->queue_lock);

	mutex_init(&fb_data->pxp_mutex);

	mutex_init(&fb_data->power_mutex);

	/* PxP DMA interface */
	dmaengine_get();

	/*
	 * Fill out PxP config data structure based on FB info and
	 * processing tasks required
	 */
	pxp_conf = &fb_data->pxp_conf;
	proc_data = &pxp_conf->proc_data;

	/* Initialize non-channel-specific PxP parameters */
	proc_data->drect.left = proc_data->srect.left = 0;
	proc_data->drect.top = proc_data->srect.top = 0;
	proc_data->drect.width = proc_data->srect.width = fb_data->info.var.xres;
	proc_data->drect.height = proc_data->srect.height = fb_data->info.var.yres;
	proc_data->scaling = 0;
	proc_data->hflip = 0;
	proc_data->vflip = 0;
	proc_data->rotate = 0;
	proc_data->bgcolor = 0;
	proc_data->overlay_state = 0;
	proc_data->lut_transform = PXP_LUT_NONE;

	/*
	 * We initially configure PxP for RGB->YUV conversion,
	 * and only write out Y component of the result.
	 */

	/*
	 * Initialize S0 channel parameters
	 * Parameters should match FB format/width/height
	 */
	pxp_conf->s0_param.pixel_fmt = PXP_PIX_FMT_RGB565;
	pxp_conf->s0_param.width = fb_data->info.var.xres_virtual;
	pxp_conf->s0_param.height = fb_data->info.var.yres;
	pxp_conf->s0_param.color_key = -1;
	pxp_conf->s0_param.color_key_enable = false;

	/*
	 * Initialize OL0 channel parameters
	 * No overlay will be used for PxP operation
	 */
	for (i = 0; i < 8; i++) {
		pxp_conf->ol_param[i].combine_enable = false;
		pxp_conf->ol_param[i].width = 0;
		pxp_conf->ol_param[i].height = 0;
		pxp_conf->ol_param[i].pixel_fmt = PXP_PIX_FMT_RGB565;
		pxp_conf->ol_param[i].color_key_enable = false;
		pxp_conf->ol_param[i].color_key = -1;
		pxp_conf->ol_param[i].global_alpha_enable = false;
		pxp_conf->ol_param[i].global_alpha = 0;
		pxp_conf->ol_param[i].local_alpha_enable = false;
	}

	/*
	 * Initialize Output channel parameters
	 * Output is Y-only greyscale
	 * Output width/height will vary based on update region size
	 */
	pxp_conf->out_param.width = fb_data->info.var.xres;
	pxp_conf->out_param.height = fb_data->info.var.yres;
	pxp_conf->out_param.pixel_fmt = PXP_PIX_FMT_GREY;

	/*
	 * Ensure this is set to NULL here...we will initialize pxp_chan
	 * later in our thread.
	 */
	fb_data->pxp_chan = NULL;

	/* Initialize Scatter-gather list containing 2 buffer addresses. */
	sg = fb_data->sg;
	sg_init_table(sg, 2);

	/*
	 * For use in PxP transfers:
	 * sg[0] holds the FB buffer pointer
	 * sg[1] holds the Output buffer pointer (configured before TX request)
	 */
	sg_dma_address(&sg[0]) = info->fix.smem_start;
	sg_set_page(&sg[0], virt_to_page(info->screen_base),
		    info->fix.smem_len, offset_in_page(info->screen_base));

	fb_data->order_cnt = 0;
	fb_data->waiting_for_wb = false;
	fb_data->waiting_for_lut = false;
	fb_data->waiting_for_lut15 = false;
	fb_data->waiting_for_idle = false;
	fb_data->blank = FB_BLANK_UNBLANK;
	fb_data->power_state = POWER_STATE_OFF;
	fb_data->powering_down = false;
/* 2011/04/19 FY11 : Modified default powerdown delay.  */
	fb_data->pwrdown_delay = DEFAULT_POWER_DOWN_DELAY;
/* 2011/04/20 FY11 : Supported to clear panel when shutdown. */
	fb_data->panel_clear_at_shutdown = false;

/* 2011/04/19 FY11 : Supported wake lock. */
	wake_lock_init( &(fb_data->wake_lock_ioctl), WAKE_LOCK_SUSPEND, "wl_epdc_ioctl" );
	wake_lock_init( &(fb_data->wake_lock_update), WAKE_LOCK_SUSPEND, "wl_epdc_update" );
	wake_lock_init( &(fb_data->wake_lock_power), WAKE_LOCK_SUSPEND, "wl_epdc_power" );
	fb_data->counter_for_wlupdate = 0;
/* 2011/07/14 FY11 : Added wake lock for A2. (Workaround for noise after sleep.) */
	wake_lock_init( &(fb_data->wake_lock_a2), WAKE_LOCK_SUSPEND, "wl_epdc_a2" );

	/* Register FB */
	ret = register_framebuffer(info);
	if (ret) {
		dev_err(&pdev->dev,
			"register_framebuffer failed with error %d\n", ret);
		goto out_dmaengine;
	}

	g_fb_data = fb_data;

	ret = device_create_file(fb_data->info.dev, &epdc_debug_attr);


/* 2011/03/08 FY11 : Initialize the pointer for temperature range table and waveform buffer. */
	fb_data->temp_range_bounds = NULL;
	fb_data->waveform_buffer_virt = NULL;
	fb_data->waveform_buffer_phys = 0;
	fb_data->waveform_buffer_size = 0;


#ifdef DEFAULT_PANEL_HW_INIT
	ret = mxc_epdc_fb_init_hw((struct fb_info *)fb_data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize HW!\n");
	}
#endif

	epdc_fb_data = fb_data;

/* 2011/04/07 FY11 : Removed showing LOGO. */

/* 2011/05/12 FY11 : Supported boot progress. */
	// start progress thread
	fb_data->epdc_progress_work_queue = create_singlethread_workqueue( "boot_progress" );
	INIT_DELAYED_WORK(&(fb_data->epdc_progress_work), epdc_progress_work_func);
	queue_delayed_work( fb_data->epdc_progress_work_queue,
			    &(fb_data->epdc_progress_work),
			    HZ / 100 );
	

	goto out;

out_dmaengine:
	dmaengine_put();
out_irq:
	free_irq(fb_data->epdc_irq, fb_data);
/* 2011/06/08 FY11 : Modified to use static buffer for waveofrm. */
out_wf_tmp_buf:
	if (fb_data->pdata->put_pins)
		fb_data->pdata->put_pins();

	if ( wf_tmp_buf )
	{
		kfree(wf_tmp_buf);
		wf_tmp_buf = NULL;
	}

out_dma_work_buf:
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
#ifdef CONFIG_EPD_STATIC_MEM_WORKBUFF
#else
	dma_free_writecombine(&pdev->dev, fb_data->working_buffer_size,
		fb_data->working_buffer_virt, fb_data->working_buffer_phys);
#endif
 out_copybuffer:
 	dma_free_writecombine(&pdev->dev, fb_data->max_pix_size*2,
 			      fb_data->virt_addr_copybuf,
 			      fb_data->phys_addr_copybuf);
out_upd_buffers:
	list_for_each_entry_safe(plist, temp_list, &fb_data->upd_buf_free_list->list, list) {
		list_del(&plist->list);
		dma_free_writecombine(&pdev->dev, fb_data->max_pix_size,
					  plist->virt_addr,
				      plist->phys_addr);
		kfree(plist);
	}
out_dma_fb:
/* 2011/03/15 FY11 : Supported standby screen. */
	dma_free_writecombine(&pdev->dev, fb_data->map_size + fb_data->standbyscreen_buff_size, info->screen_base,
			      fb_data->phys_start);

out_mapregs:
	iounmap(epdc_base);
out_cmap:
	fb_dealloc_cmap(&info->cmap);
out_fbdata:
	kfree(fb_data);
out:
	return ret;
}

static int mxc_epdc_fb_remove(struct platform_device *pdev)
{
	struct update_data_list *plist, *temp_list;
	struct mxc_epdc_fb_data *fb_data = platform_get_drvdata(pdev);

	epdc_fb_data = NULL;
	/* wait starting of all panel update. */
	mxc_epdc_fb_blank(FB_BLANK_POWERDOWN, &fb_data->info);

	flush_workqueue(fb_data->epdc_submit_workqueue);
	destroy_workqueue(fb_data->epdc_submit_workqueue);

	regulator_put(fb_data->display_regulator);
	regulator_put(fb_data->vcom_regulator);
	regulator_put(fb_data->epd_pwr0_regulator);
	regulator_put(fb_data->epd_pwr2_regulator);
	regulator_put(fb_data->temp_regulator);

	unregister_framebuffer(&fb_data->info);
	free_irq(fb_data->epdc_irq, fb_data);

/* 2011/04/19 FY11 : Supported wake lock. */
	wake_lock_destroy( &(fb_data->wake_lock_ioctl) );
	wake_lock_destroy( &(fb_data->wake_lock_update) );
	wake_lock_destroy( &(fb_data->wake_lock_power) );

/* 2011/06/08 FY11 : Modified to use static buffer for waveofrm. */
	if ( wf_tmp_buf )
	{
		kfree(wf_tmp_buf);
		wf_tmp_buf = NULL;
	}

/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
#ifdef CONFIG_EPD_STATIC_MEM_WORKBUFF
#else
	dma_free_writecombine(&pdev->dev, fb_data->working_buffer_size,
				fb_data->working_buffer_virt,
				fb_data->working_buffer_phys);
#endif

	if (fb_data->waveform_buffer_virt != NULL)
	{
/* 2011/06/07 FY11 : Modified to share static memory for epdc with bootloader. */
#ifdef CONFIG_EPD_STATIC_MEM_WAVEFORM
#else
		dma_free_writecombine(&pdev->dev, fb_data->waveform_buffer_size,
				fb_data->waveform_buffer_virt,
				fb_data->waveform_buffer_phys);
#endif
	}

	if (fb_data->virt_addr_copybuf != NULL)
		dma_free_writecombine(&pdev->dev, fb_data->max_pix_size*2,
				fb_data->virt_addr_copybuf,
				fb_data->phys_addr_copybuf);
	list_for_each_entry_safe(plist, temp_list, &fb_data->upd_buf_free_list->list, list) {
		list_del(&plist->list);
 		dma_free_writecombine(&pdev->dev, fb_data->max_pix_size,
  				      plist->virt_addr,
  				      plist->phys_addr);
		kfree(plist);
	}
#ifdef CONFIG_FB_MXC_EINK_AUTO_UPDATE_MODE
	fb_deferred_io_cleanup(&fb_data->info);
#endif

/* 2011/03/15 FY11 : Supported standby screen. */
	dma_free_writecombine(&pdev->dev, fb_data->map_size + fb_data->standbyscreen_buff_size, fb_data->info.screen_base,
			      fb_data->phys_start);

	if (fb_data->pdata->put_pins)
		fb_data->pdata->put_pins();

	/* Release PxP-related resources */
	if (fb_data->pxp_chan != NULL)
		dma_release_channel(&fb_data->pxp_chan->dma_chan);

/* 2011/03/08 FY11 : Supported to write waveform. */
	if ( fb_data->temp_range_bounds )
	{
		kzfree(fb_data->temp_range_bounds);
		fb_data->temp_range_bounds = NULL;
	}

	dmaengine_put();

	iounmap(epdc_base);

	fb_dealloc_cmap(&fb_data->info.cmap);

	framebuffer_release(&fb_data->info);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

#ifdef CONFIG_EARLYSUSPEND
static void mxc_epdc_early_suspend(struct early_suspend *h)
{
/* 2011/03/09 FY11 : Fixed the bug that CPU hangs caused by accessing to EPDC without clock. */
/* 2011/03/15 FY11 : Supported standby screen. */
	struct mxcfb_update_data stUpdateData;
	struct fb_info *info;
	int ret = 0;

	epdc_debug_printk( "mode=%d\n", h->pm_mode );

	if (!epdc_fb_data)
		return;

	info = (struct fb_info*)epdc_fb_data;

	if (h->pm_mode == EARLY_SUSPEND_MODE_NORMAL)
	{
		mutex_lock(&info->lock);

		//printk( KERN_ERR "%s mode=EARLY_SUSPEND_MODE_NORMAL\n", __func__ );

/* 2011/06/21 FY11 : Added black screen. */
		// wait for completion of all previous update request.
		ret = mxc_epdc_fb_wait_update_complete( 0, info );
		if ( ret < 0 )
		{
			printk(KERN_ERR "%s fails to wait completion of all previous update.\n", __func__ );
		}

		// cancel power down delayed work and power up
		cancel_delayed_work( &(epdc_fb_data->epdc_done_work) );
		epdc_powerup(epdc_fb_data);

		// insert black screen
		stUpdateData.update_region.top = 0;
		stUpdateData.update_region.left = 0;
		stUpdateData.update_region.width = epdc_fb_data->native_width;
		stUpdateData.update_region.height = epdc_fb_data->native_height;
		stUpdateData.waveform_mode = epdc_fb_data->wv_modes.mode_gc16;
		stUpdateData.update_mode = UPDATE_MODE_FULL;
		epdc_draw_rect( epdc_fb_data, &stUpdateData, 0x00, true );


		// set parameter
		stUpdateData.update_region.top = 0;
		stUpdateData.update_region.left = 0;
		stUpdateData.update_region.width = epdc_fb_data->info.var.xres;
		stUpdateData.update_region.height = epdc_fb_data->info.var.yres;
		stUpdateData.waveform_mode = epdc_fb_data->wv_modes.mode_gc16;
		stUpdateData.update_mode = UPDATE_MODE_FULL;
		stUpdateData.update_marker = SSCREEN_UPDATE_MARKER;
		stUpdateData.temp = TEMP_USE_AMBIENT;
		stUpdateData.flags = 0;

		ret = mxc_epdc_fb_send_update( &stUpdateData, &(epdc_fb_data->info), true );
		if ( ret < 0 )
		{
			printk (KERN_ERR "%s send_update failed. %d\n", __func__, ret );
			mutex_unlock(&info->lock);
			return;
		}

		ret = mxc_epdc_fb_wait_update_complete( stUpdateData.update_marker, &(epdc_fb_data->info));
		if ( ret < 0 )
		{
			printk (KERN_ERR "%s wait complete failed. %d\n", __func__, ret );
			mutex_unlock(&info->lock);
			return;
		}

		// cancel power down delayed work and power down 
		cancel_delayed_work( &(epdc_fb_data->epdc_done_work) );
		epdc_fb_data->powering_down = true;
		epdc_powerdown(epdc_fb_data);

		mutex_unlock(&info->lock);

	}
}

/* 2011/03/15 FY11 : Supported standby screen. */
static void mxc_epdc_late_resume(struct early_suspend *h)
{
	struct mxcfb_update_data stUpdateData;
	struct fb_info *info;
	int ret;

	epdc_debug_printk( "mode=%d\n", h->pm_mode );

	if (!epdc_fb_data)
		return;

	if (h->pm_mode == EARLY_SUSPEND_MODE_NORMAL) 
	{
		//printk( KERN_ERR "%s mode=EARLY_SUSPEND_MODE_NORMAL\n", __func__ );

		info = (struct fb_info*)epdc_fb_data;
		// wait for early suspend completion.
		mutex_lock(&info->lock);

		flush_workqueue(epdc_fb_data->epdc_submit_workqueue);

/* 2011/06/21 FY11 : Added black screen. */
		// powerup
		epdc_powerup(epdc_fb_data);

		// insert black screen
		stUpdateData.update_region.top = 0;
		stUpdateData.update_region.left = 0;
		stUpdateData.update_region.width = epdc_fb_data->native_width;
		stUpdateData.update_region.height = epdc_fb_data->native_height;
		stUpdateData.waveform_mode = epdc_fb_data->wv_modes.mode_gc16;
		stUpdateData.update_mode = UPDATE_MODE_FULL;
		epdc_draw_rect( epdc_fb_data, &stUpdateData, 0x00, true );

/* 2011/07/07 FY11 : Removed restoring FB and added clearing FB. */
		memset( info->screen_base, 0x00, epdc_fb_data->map_size  );

		mutex_unlock(&info->lock);
	}
}

static struct early_suspend mxc_epdc_earlysuspend = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = mxc_epdc_early_suspend,
	.resume = mxc_epdc_late_resume,
};
#endif

#ifdef CONFIG_PM
static int mxc_epdc_fb_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct mxc_epdc_fb_data *data = platform_get_drvdata(pdev);
	int ret;
	ret = mxc_epdc_fb_blank(FB_BLANK_POWERDOWN, &data->info);
	if (ret)
		goto out;

out:
	return ret;
}

static int mxc_epdc_fb_resume(struct platform_device *pdev)
{
	struct mxc_epdc_fb_data *data = platform_get_drvdata(pdev);

	mxc_epdc_fb_blank(FB_BLANK_UNBLANK, &data->info);
	return 0;
}
#else
#define mxc_epdc_fb_suspend	NULL
#define mxc_epdc_fb_resume	NULL
#endif

/* 2011/04/20 FY11 : Supported to clear panel when shutdown. */
static void mxc_epdc_fb_shutdown(struct platform_device *pdev)
{
	int i, retry=0, white_lut;
	struct mxc_epdc_fb_data *fb_data = platform_get_drvdata(pdev);

	epdc_debug_printk( "start" );
	mutex_lock(&(fb_data->info.lock));

	// wait starting of all panel update requested by application.
	mxc_epdc_fb_blank( FB_BLANK_POWERDOWN, &(fb_data->info) );

	epdc_debug_printk( "power up\n" );
	fb_data->pwrdown_delay = FB_POWERDOWN_DISABLE;
	cancel_delayed_work( &(fb_data->epdc_done_work) );
	epdc_powerup(fb_data);

	// if panel init flag is not set, display white image.
	if ( fb_data->panel_clear_at_shutdown )
	{
		if ( !epdc_any_luts_available() )
		{
			epdc_debug_printk( "wait lut.\n" );
			/* Initialize event signalling an update resource is free */
			init_completion(&fb_data->update_res_free);
			fb_data->waiting_for_lut = true;
			wait_for_completion(&fb_data->update_res_free);
		}
		white_lut = epdc_get_next_lut();

		epdc_debug_printk( "read temp.\n" );
		mxc_epdc_fb_read_temperature(fb_data);
		epdc_set_temp(fb_data->temp_index);
	
		epdc_set_update_coord(0, 0);
		epdc_set_update_dimensions( fb_data->cur_mode->vmode->xres,
					    fb_data->cur_mode->vmode->yres );
		epdc_debug_printk( "start display black.\n" );
		epdc_submit_update(white_lut,
				   fb_data->wv_modes.mode_gc16,
				   UPDATE_MODE_FULL, 
				   true, 0);
		// wait black display
		retry = 30;
		while( epdc_is_lut_active(white_lut) ||
			epdc_is_working_buffer_busy() )
		{
			msleep(100);
			retry--;
			if ( retry == 0 )
			{
				printk( KERN_ERR "%s  black complete timeout.\n", __func__ );
				goto out;
			}
		}

		epdc_debug_printk( "start clear panel.\n" );
		epdc_submit_update(white_lut,
				   fb_data->wv_modes.mode_gc16,
				   UPDATE_MODE_FULL, 
				   true, 0xFF);
	}

	epdc_debug_printk( "wait lut completion\n" );

	// wait all update completion.
	for ( i = 0; i < EPDC_NUM_LUTS; i++ )
	{
		retry = 30;
		while( epdc_is_lut_active(i) ||
			epdc_is_working_buffer_busy() )
		{
			msleep(100);
			retry--;
			if ( retry == 0 )
			{
				printk( KERN_ERR "%s wait lut %d idle timeout.\n", __func__, i );
				break;
			}
		}
	}

out:
	epdc_debug_printk( "powerdown\n" );
	fb_data->powering_down = true;
	epdc_powerdown(fb_data);

	epdc_debug_printk( "end" );
}


static struct platform_driver mxc_epdc_fb_driver = {
	.probe = mxc_epdc_fb_probe,
/* 2011/04/20 FY11 : Supported to clear panel when shutdown. */
	.shutdown = mxc_epdc_fb_shutdown,
	.remove = mxc_epdc_fb_remove,
	.suspend = mxc_epdc_fb_suspend,
	.resume = mxc_epdc_fb_resume,
	.driver = {
		   .name = "mxc_epdc_fb",
		   .owner = THIS_MODULE,
		   },
};

/* Callback function triggered after PxP receives an EOF interrupt */
static void pxp_dma_done(void *arg)
{
	struct pxp_tx_desc *tx_desc = to_tx_desc(arg);
	struct dma_chan *chan = tx_desc->txd.chan;
	struct pxp_channel *pxp_chan = to_pxp_channel(chan);
	struct mxc_epdc_fb_data *fb_data = pxp_chan->client;

	/* This call will signal wait_for_completion_timeout() in send_buffer_to_pxp */
	complete(&fb_data->pxp_tx_cmpl);
}

/* Function to request PXP DMA channel */
static int pxp_chan_init(struct mxc_epdc_fb_data *fb_data)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;

	/*
	 * Request a free channel
	 */
	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_cap_set(DMA_PRIVATE, mask);
	chan = dma_request_channel(mask, NULL, NULL);
	if (!chan) {
		dev_err(fb_data->dev, "Unsuccessfully received channel!!!!\n");
		return -EBUSY;
	}

	dev_dbg(fb_data->dev, "Successfully received channel.\n");

	fb_data->pxp_chan = to_pxp_channel(chan);

	fb_data->pxp_chan->client = fb_data;

	init_completion(&fb_data->pxp_tx_cmpl);

	return 0;
}

/*
 * Function to call PxP DMA driver and send our latest FB update region
 * through the PxP and out to an intermediate buffer.
 * Note: This is a blocking call, so upon return the PxP tx should be complete.
 */
static int pxp_process_update(struct mxc_epdc_fb_data *fb_data,
			      u32 src_width, u32 src_height,
			      struct mxcfb_rect *update_region)
{
	dma_cookie_t cookie;
	struct scatterlist *sg = fb_data->sg;
	struct dma_chan *dma_chan;
	struct pxp_tx_desc *desc;
	struct dma_async_tx_descriptor *txd;
	struct pxp_config_data *pxp_conf = &fb_data->pxp_conf;
	struct pxp_proc_data *proc_data = &fb_data->pxp_conf.proc_data;
	int i, ret;
	int length;

	dev_dbg(fb_data->dev, "Starting PxP Send Buffer\n");

	/* First, check to see that we have acquired a PxP Channel object */
	if (fb_data->pxp_chan == NULL) {
		/*
		 * PxP Channel has not yet been created and initialized,
		 * so let's go ahead and try
		 */
		ret = pxp_chan_init(fb_data);
		if (ret) {
			/*
			 * PxP channel init failed, and we can't use the
			 * PxP until the PxP DMA driver has loaded, so we abort
			 */
			dev_err(fb_data->dev, "PxP chan init failed\n");
			return -ENODEV;
		}
	}

	/*
	 * Init completion, so that we
	 * can be properly informed of the completion
	 * of the PxP task when it is done.
	 */
	init_completion(&fb_data->pxp_tx_cmpl);

	dev_dbg(fb_data->dev, "sg[0] = 0x%x, sg[1] = 0x%x\n",
		sg_dma_address(&sg[0]), sg_dma_address(&sg[1]));

	dma_chan = &fb_data->pxp_chan->dma_chan;

	txd = dma_chan->device->device_prep_slave_sg(dma_chan, sg, 2,
						     DMA_TO_DEVICE,
						     DMA_PREP_INTERRUPT);
	if (!txd) {
		dev_err(fb_data->info.device,
			"Error preparing a DMA transaction descriptor.\n");
		return -EIO;
	}

	txd->callback_param = txd;
	txd->callback = pxp_dma_done;

	/*
	 * Configure PxP for processing of new update region
	 * The rest of our config params were set up in
	 * probe() and should not need to be changed.
	 */
	pxp_conf->s0_param.width = src_width;
	pxp_conf->s0_param.height = src_height;
	proc_data->srect.top = update_region->top;
	proc_data->srect.left = update_region->left;
	proc_data->srect.width = update_region->width;
	proc_data->srect.height = update_region->height;

	/*
	 * Because only YUV/YCbCr image can be scaled, configure
	 * drect equivalent to srect, as such do not perform scaling.
	 */
	proc_data->drect.top = 0;
	proc_data->drect.left = 0;
	proc_data->drect.width = proc_data->srect.width;
	proc_data->drect.height = proc_data->srect.height;

	/* PXP expects rotation in terms of degrees */
	proc_data->rotate = fb_data->epdc_fb_var.rotate * 90;
	if (proc_data->rotate > 270)
		proc_data->rotate = 0;

	pxp_conf->out_param.width = update_region->width;
	pxp_conf->out_param.height = update_region->height;

	desc = to_tx_desc(txd);
	length = desc->len;
	for (i = 0; i < length; i++) {
		if (i == 0) {/* S0 */
			memcpy(&desc->proc_data, proc_data, sizeof(struct pxp_proc_data));
			pxp_conf->s0_param.paddr = sg_dma_address(&sg[0]);
			memcpy(&desc->layer_param.s0_param, &pxp_conf->s0_param,
				sizeof(struct pxp_layer_param));
		} else if (i == 1) {
			pxp_conf->out_param.paddr = sg_dma_address(&sg[1]);
			memcpy(&desc->layer_param.out_param, &pxp_conf->out_param,
				sizeof(struct pxp_layer_param));
		}
		/* TODO: OverLay */

		desc = desc->next;
	}

	/* Submitting our TX starts the PxP processing task */
	cookie = txd->tx_submit(txd);
	dev_dbg(fb_data->info.device, "%d: Submit %p #%d\n", __LINE__, txd,
		cookie);
	if (cookie < 0) {
		dev_err(fb_data->info.device, "Error sending FB through PxP\n");
		return -EIO;
	}

	fb_data->txd = txd;

	/* trigger ePxP */
	dma_async_issue_pending(dma_chan);

	return 0;
}

static int pxp_complete_update(struct mxc_epdc_fb_data *fb_data, u32 *hist_stat)
{
	int ret;
	/*
	 * Wait for completion event, which will be set
	 * through our TX callback function.
	 */
	ret = wait_for_completion_timeout(&fb_data->pxp_tx_cmpl, HZ / 2);
	if (ret <= 0) {
		dev_info(fb_data->info.device,
			 "PxP operation failed due to %s\n",
			 ret < 0 ? "user interrupt" : "timeout");
		dma_release_channel(&fb_data->pxp_chan->dma_chan);
		fb_data->pxp_chan = NULL;
		return ret ? : -ETIMEDOUT;
	}

	*hist_stat = to_tx_desc(fb_data->txd)->hist_status;
	dma_release_channel(&fb_data->pxp_chan->dma_chan);
	fb_data->pxp_chan = NULL;

	dev_dbg(fb_data->dev, "TX completed\n");

	return 0;
}

static int __init mxc_epdc_fb_init(void)
{
#ifdef CONFIG_EARLYSUSPEND
/* 2011/03/15 FY11 : Supported standby screen. */
	register_early_suspend(&mxc_epdc_earlysuspend);
#endif
	return platform_driver_register(&mxc_epdc_fb_driver);
}
late_initcall(mxc_epdc_fb_init);


static void __exit mxc_epdc_fb_exit(void)
{
	platform_driver_unregister(&mxc_epdc_fb_driver);
#ifdef CONFIG_EARLYSUSPEND
/* 2011/03/15 FY11 : Supported standby screen. */
	unregister_early_suspend(&mxc_epdc_earlysuspend);
#endif
}
module_exit(mxc_epdc_fb_exit);

MODULE_AUTHOR("Freescale Semiconductor, Inc.");
MODULE_DESCRIPTION("MXC EPDC framebuffer driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("fb");
