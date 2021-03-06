/*
 * Copyright (C) 2008-2011 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#ifndef __ARCH_ARM_MACH_MX51_SERIAL_H__
#define __ARCH_ARM_MACH_MX51_SERIAL_H__

/* UART 1 configuration */
/*!
 * This specifies the threshold at which the CTS pin is deasserted by the
 * RXFIFO. Set this value in Decimal to anything from 0 to 32 for
 * hardware-driven hardware flow control. Read the HW spec while specifying
 * this value. When using interrupt-driven software controlled hardware
 * flow control set this option to -1.
 */
#define UART1_UCR4_CTSTL        16
/*!
 * Specify the size of the DMA receive buffer. The minimum buffer size is 512
 * bytes. The buffer size should be a multiple of 256.
 */
#define UART1_DMA_RXBUFSIZE     1024
/*!
 * Specify the MXC UART's Receive Trigger Level. This controls the threshold at
 * which a maskable interrupt is generated by the RxFIFO. Set this value in
 * Decimal to anything from 0 to 32. Read the HW spec while specifying this
 * value.
 */
#define UART1_UFCR_RXTL         16
/*!
 * Specify the MXC UART's Transmit Trigger Level. This controls the threshold at
 * which a maskable interrupt is generated by the TxFIFO. Set this value in
 * Decimal to anything from 0 to 32. Read the HW spec while specifying this
 * value.
 */
#define UART1_UFCR_TXTL         16
#define UART1_DMA_ENABLE	0
/* UART 2 configuration */
#define UART2_UCR4_CTSTL        -1
#if 1 /* E_BOOK *//* for IR touchpanel 2011/1/19 */
#define UART2_DMA_ENABLE	0
#else
#define UART2_DMA_ENABLE	1
#endif
#define UART2_DMA_RXBUFSIZE     512
#define UART2_UFCR_RXTL         16
#define UART2_UFCR_TXTL         16
/* UART 3 configuration */
#define UART3_UCR4_CTSTL        16
#define UART3_DMA_ENABLE	0
#define UART3_DMA_RXBUFSIZE     1024
#define UART3_UFCR_RXTL         16
#define UART3_UFCR_TXTL         16
/* UART 4 configuration */
#define UART4_UCR4_CTSTL        -1
#define UART4_DMA_ENABLE	0
#define UART4_DMA_RXBUFSIZE     512
#define UART4_UFCR_RXTL         16
#define UART4_UFCR_TXTL         16
/* UART 5 configuration */
#define UART5_UCR4_CTSTL        -1
#define UART5_DMA_ENABLE	0
#define UART5_DMA_RXBUFSIZE     512
#define UART5_UFCR_RXTL         16
#define UART5_UFCR_TXTL         16

#endif				/* __ARCH_ARM_MACH_MX51_SERIAL_H__ */
