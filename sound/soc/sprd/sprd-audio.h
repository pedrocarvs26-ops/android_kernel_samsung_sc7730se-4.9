/*
 * Spreadtrum SC8830-family audio glue: VBC and codec clocks, resets,
 * ownership switching and codec register access.
 *
 * Based on the vendor 3.10 <mach/__sprd_audio_sc8830.h> and
 * <mach/__sprd_audio_power.h>.
 *
 * Copyright (C) 2013 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef __SOUND_SOC_SPRD_AUDIO_H
#define __SOUND_SOC_SPRD_AUDIO_H

#include <linux/delay.h>
#include <linux/errno.h>
#include <soc/sprd/dma.h>
#include <soc/sprd/iomap.h>

enum {
	AUDIO_NO_CHANGE,
	AUDIO_TO_CP0_DSP_CTRL,
	AUDIO_TO_CP1_DSP_CTRL,
	AUDIO_TO_AP_ARM_CTRL,
	AUDIO_TO_ARM_CTRL = AUDIO_TO_AP_ARM_CTRL,
	AUDIO_TO_CP0_ARM_CTRL,
	AUDIO_TO_CP1_ARM_CTRL,
	AUDIO_TO_CP2_ARM_CTRL,
};

#define VBC_BASE		SPRD_VBC_BASE
#define VBC_PHY_BASE		SPRD_VBC_PHYS

/* VBC DAC buffers as seen from the CP2 (WCN) side */
#define VBC_CP2_PHY_BASE	0x02020000
#define CP2_PHYS_VBDA0		(VBC_CP2_PHY_BASE + 0x0000)
#define CP2_PHYS_VBDA1		(VBC_CP2_PHY_BASE + 0x0004)

#define CODEC_DP_BASE		SPRD_AUDIO_BASE
#define CODEC_DP_PHY_BASE	SPRD_AUDIO_PHYS

/*
 * The codec driver keeps register addresses in 16 bits and ORs the upper
 * half of the base back in on access, so bit 15 of the analog base can't
 * live in the low half. Strip it from the base and add it back as an
 * offset in the accessors below.
 */
#define CODEC_AP_BASE		(ANA_AUDCFGA_INT_BASE & ~0x8000UL)
#define CODEC_AP_OFFSET		(ANA_AUDCFGA_INT_BASE & 0x8000UL)
#define CODEC_AP_PHY_BASE	(SPRD_ADISLAVE_PHYS + 0x0600)

#define CLASS_G_LDO_ID		"vddclsg"

/* SC2723: 0x2723 in the top half, metal revision in the low 12 bits */
#define ANA_CHIP_VER_MASK	0xFFF

static inline u32 sci_get_ana_chip_id(void)
{
	u32 high = sci_adi_read(ANA_REG_GLB_CHIP_ID_HIGH) & 0xFFFF;
	u32 low = sci_adi_read(ANA_REG_GLB_CHIP_ID_LOW) & 0xFFFF;

	return (high << 16) | (low & ~ANA_CHIP_VER_MASK);
}

static inline u32 sci_get_ana_chip_ver(void)
{
	return sci_adi_read(ANA_REG_GLB_CHIP_ID_LOW) & ANA_CHIP_VER_MASK;
}

/* VBC */
static inline int arch_audio_vbc_reg_enable(void)
{
	return sci_glb_set(REG_AON_APB_APB_EB0, BIT_VBC_EB);
}

static inline int arch_audio_vbc_reg_disable(void)
{
	return sci_glb_clr(REG_AON_APB_APB_EB0, BIT_VBC_EB);
}

static inline int arch_audio_vbc_enable(void)
{
	return 0;
}

static inline int arch_audio_vbc_disable(void)
{
	return 0;
}

static inline int arch_audio_vbc_reset(void)
{
	sci_glb_set(REG_AON_APB_APB_RST0, BIT_VBC_SOFT_RST);
	udelay(10);
	sci_glb_clr(REG_AON_APB_APB_RST0, BIT_VBC_SOFT_RST);
	return 0;
}

#define VBC_SYS_SEL(x)	(BITS_VBC_AFIFO_INT_SYS_SEL(x) |	\
			 BITS_VBC_DA01_INT_SYS_SEL(x) |		\
			 BITS_VBC_AD01_INT_SYS_SEL(x) |		\
			 BITS_VBC_AD23_INT_SYS_SEL(x) |		\
			 BITS_VBC_DA01_DMA_SYS_SEL(x) |		\
			 BITS_VBC_AD01_DMA_SYS_SEL(x) |		\
			 BITS_VBC_AD23_DMA_SYS_SEL(x))

/*
 * Hand the VBC interrupts and DMA requests to a subsystem. With
 * AUDIO_NO_CHANGE, return the current owner instead.
 */
static inline int arch_audio_vbc_switch(int master)
{
	u32 cp0_arm = BIT_VBC_INT_CP0_ARM_SEL | BIT_VBC_DMA_CP0_ARM_SEL;
	u32 cp1_arm = BIT_VBC_INT_CP1_ARM_SEL | BIT_VBC_DMA_CP1_ARM_SEL;
	u32 val;

	if (master != AUDIO_NO_CHANGE) {
		sci_glb_set(REG_AON_APB_CP0_WPROT_EN, BIT(6));
		sci_glb_set(REG_AON_APB_CP1_WPROT_EN, BIT(6));
		sci_glb_set(REG_AON_APB_CP2_WPROT_EN, BIT(6));
	}

	switch (master) {
	case AUDIO_TO_AP_ARM_CTRL:
		sci_glb_write(REG_AON_APB_VBC_CTRL, VBC_SYS_SEL(0),
			      VBC_SYS_SEL(3));
		sci_glb_clr(REG_AON_APB_AP_WPROT_EN, BIT(6));
		arch_audio_vbc_reset();
		arch_audio_vbc_reg_disable();
		break;
	case AUDIO_TO_CP0_DSP_CTRL:
		arch_audio_vbc_reset();
		arch_audio_vbc_reg_enable();
		sci_glb_clr(REG_AON_APB_CP0_WPROT_EN, BIT(6));
		sci_glb_write(REG_AON_APB_VBC_CTRL, VBC_SYS_SEL(1),
			      VBC_SYS_SEL(3) | cp0_arm);
		break;
	case AUDIO_TO_CP1_DSP_CTRL:
		arch_audio_vbc_reset();
		arch_audio_vbc_reg_enable();
		sci_glb_clr(REG_AON_APB_CP1_WPROT_EN, BIT(6));
		sci_glb_write(REG_AON_APB_VBC_CTRL, VBC_SYS_SEL(2),
			      VBC_SYS_SEL(3) | cp1_arm);
		break;
	case AUDIO_TO_CP0_ARM_CTRL:
		arch_audio_vbc_reset();
		arch_audio_vbc_reg_enable();
		sci_glb_clr(REG_AON_APB_CP0_WPROT_EN, BIT(6));
		sci_glb_write(REG_AON_APB_VBC_CTRL, VBC_SYS_SEL(1) | cp0_arm,
			      VBC_SYS_SEL(3) | cp0_arm);
		break;
	case AUDIO_TO_CP1_ARM_CTRL:
		arch_audio_vbc_reset();
		arch_audio_vbc_reg_enable();
		sci_glb_clr(REG_AON_APB_CP1_WPROT_EN, BIT(6));
		sci_glb_write(REG_AON_APB_VBC_CTRL, VBC_SYS_SEL(2) | cp1_arm,
			      VBC_SYS_SEL(3) | cp1_arm);
		break;
	case AUDIO_TO_CP2_ARM_CTRL:
		sci_glb_clr(REG_AON_APB_CP2_WPROT_EN, BIT(6));
		sci_glb_write(REG_AON_APB_VBC_CTRL, VBC_SYS_SEL(3),
			      VBC_SYS_SEL(3));
		break;
	case AUDIO_NO_CHANGE:
		val = sci_glb_read(REG_AON_APB_VBC_CTRL,
				   BITS_VBC_DA01_INT_SYS_SEL(3));
		if (val == BITS_VBC_DA01_INT_SYS_SEL(0))
			return AUDIO_TO_AP_ARM_CTRL;
		if (val == BITS_VBC_DA01_INT_SYS_SEL(1))
			return sci_glb_read(REG_AON_APB_VBC_CTRL,
					    BIT_VBC_INT_CP0_ARM_SEL) ?
			       AUDIO_TO_CP0_ARM_CTRL : AUDIO_TO_CP0_DSP_CTRL;
		if (val == BITS_VBC_DA01_INT_SYS_SEL(2))
			return sci_glb_read(REG_AON_APB_VBC_CTRL,
					    BIT_VBC_INT_CP1_ARM_SEL) ?
			       AUDIO_TO_CP1_ARM_CTRL : AUDIO_TO_CP1_DSP_CTRL;
		return AUDIO_TO_CP2_ARM_CTRL;
	default:
		return -ENODEV;
	}

	return 0;
}

static inline int arch_audio_vbc_da_dma_info(int chan)
{
	switch (chan) {
	case 0:
		return DMA_VB_DA0;
	case 1:
		return DMA_VB_DA1;
	default:
		return -ENODEV;
	}
}

static inline int arch_audio_vbc_ad_dma_info(int chan)
{
	switch (chan) {
	case 0:
		return DMA_VB_AD0;
	case 1:
		return DMA_VB_AD1;
	default:
		return -ENODEV;
	}
}

static inline int arch_audio_vbc_ad23_dma_info(int chan)
{
	switch (chan) {
	case 0:
		return DMA_VB_AD2;
	case 1:
		return DMA_VB_AD3;
	default:
		return -ENODEV;
	}
}

/* codec register access, through the ADI bus */
static inline int arch_audio_codec_write_mask(int reg, int val, int mask)
{
	return sci_adi_write(reg + CODEC_AP_OFFSET, val, mask);
}

static inline int arch_audio_codec_write(int reg, int val)
{
	return sci_adi_write(reg + CODEC_AP_OFFSET, val, 0xFFFF);
}

static inline int arch_audio_codec_read(int reg)
{
	return sci_adi_read(reg + CODEC_AP_OFFSET);
}

static inline int arch_audio_power_write_mask(int reg, int val, int mask)
{
	return arch_audio_codec_write_mask(reg, val, mask);
}

static inline int arch_audio_power_write(int reg, int val)
{
	return arch_audio_codec_write(reg, val);
}

static inline int arch_audio_power_read(int reg)
{
	return arch_audio_codec_read(reg);
}

/* codec clocks and resets */
static inline int arch_audio_codec_audif_enable(int auto_clk)
{
	if (auto_clk) {
		sci_glb_clr(REG_AON_APB_APB_EB0, BIT_AUDIF_EB);
		sci_glb_set(REG_AON_APB_VBC_CTRL, BIT_AUDIF_CKG_AUTO_EN);
	} else {
		sci_glb_set(REG_AON_APB_APB_EB0, BIT_AUDIF_EB);
		sci_glb_clr(REG_AON_APB_VBC_CTRL, BIT_AUDIF_CKG_AUTO_EN);
	}
	return 0;
}

static inline int arch_audio_codec_audif_disable(void)
{
	sci_glb_clr(REG_AON_APB_APB_EB0, BIT_AUDIF_EB);
	sci_glb_clr(REG_AON_APB_VBC_CTRL, BIT_AUDIF_CKG_AUTO_EN);
	return 0;
}

static inline int arch_audio_codec_digital_reg_enable(void)
{
	sci_glb_set(REG_AON_APB_APB_EB0, BIT_AUD_EB);
	return arch_audio_codec_audif_enable(1);
}

static inline int arch_audio_codec_digital_reg_disable(void)
{
	arch_audio_codec_audif_disable();
	return sci_glb_clr(REG_AON_APB_APB_EB0, BIT_AUD_EB);
}

static inline int arch_audio_codec_analog_reg_enable(void)
{
	int ret;

	ret = sci_adi_write(ANA_REG_GLB_ARM_MODULE_EN, BIT_ANA_AUD_EN,
			    BIT_ANA_AUD_EN);
	if (ret < 0)
		return ret;

	/* don't let sleep control power the audio block down */
	return sci_adi_write(ANA_REG_GLB_AUD_SLP_CTRL, 0, 0xFFFF);
}

static inline int arch_audio_codec_analog_reg_disable(void)
{
	return sci_adi_write(ANA_REG_GLB_ARM_MODULE_EN, 0, BIT_ANA_AUD_EN);
}

static inline int arch_audio_codec_analog_enable(void)
{
	u16 mask = BIT_CLK_AUD_6P5M_EN | BIT_CLK_AUDIF_EN;

	/* AUDIF and 6.5 MHz clocks */
	sci_adi_write(ANA_REG_GLB_ARM_CLK_EN, mask, mask);
	sci_adi_write(ANA_REG_GLB_AUDIO_CTRL0, BIT_CLK_AUD_6P5M_TX_INV_EN,
		      BIT_CLK_AUD_6P5M_TX_INV_EN);
	/* the SC2723S must not reuse ADSYNC */
	sci_adi_write(ANA_AUDIFA_INT_BASE, 0, BIT(4));
	/* RTC and 26 MHz clocks */
	sci_adi_write(ANA_REG_GLB_RTC_CLK_EN, BIT_RTC_AUD_EN, BIT_RTC_AUD_EN);
	sci_adi_write(ANA_REG_GLB_XTL_WAIT_CTRL, BIT_XTL_EN, BIT_XTL_EN);
	/* deep sleep must not force the audio LDOs off */
	sci_adi_write(ANA_REG_GLB_AUD_SLP_CTRL, 0, 0xFFFF);
	return 0;
}

static inline int arch_audio_codec_analog_disable(void)
{
	u16 mask = BIT_CLK_AUD_6P5M_EN | BIT_CLK_AUDIF_EN;

	sci_adi_write(ANA_REG_GLB_ARM_CLK_EN, 0, mask);
	sci_adi_write(ANA_REG_GLB_AUDIO_CTRL0, BIT_CLK_AUD_6P5M_TX_INV_EN,
		      BIT_CLK_AUD_6P5M_TX_INV_EN);
	sci_adi_write(ANA_REG_GLB_RTC_CLK_EN, 0, BIT_RTC_AUD_EN);
	/* the 26 MHz clock is shared with the ADC, leave it running */
	return 0;
}

static inline int arch_audio_codec_digital_enable(void)
{
	/* internal digital 26 MHz */
	return sci_glb_write(REG_AON_APB_SINDRV_CTRL, BIT_SINDRV_ENA,
			     BIT_SINDRV_ENA);
}

static inline int arch_audio_codec_digital_disable(void)
{
	return sci_glb_write(REG_AON_APB_SINDRV_CTRL, 0, BIT_SINDRV_ENA);
}

static inline int arch_audio_codec_switch(int master)
{
	u32 val;

	switch (master) {
	case AUDIO_TO_AP_ARM_CTRL:
		val = BITS_AUD_INT_SYS_SEL(0);
		break;
	case AUDIO_TO_CP0_ARM_CTRL:
		val = BITS_AUD_INT_SYS_SEL(1);
		break;
	case AUDIO_TO_CP1_ARM_CTRL:
		val = BITS_AUD_INT_SYS_SEL(2);
		break;
	case AUDIO_TO_CP2_ARM_CTRL:
		val = BITS_AUD_INT_SYS_SEL(3);
		break;
	case AUDIO_NO_CHANGE:
		val = sci_glb_read(REG_AON_APB_VBC_CTRL,
				   BITS_AUD_INT_SYS_SEL(3));
		if (val == BITS_AUD_INT_SYS_SEL(0))
			return AUDIO_TO_AP_ARM_CTRL;
		if (val == BITS_AUD_INT_SYS_SEL(1))
			return AUDIO_TO_CP0_ARM_CTRL;
		if (val == BITS_AUD_INT_SYS_SEL(2))
			return AUDIO_TO_CP1_ARM_CTRL;
		return AUDIO_TO_CP2_ARM_CTRL;
	default:
		return -ENODEV;
	}

	return sci_glb_write(REG_AON_APB_VBC_CTRL, val,
			     BITS_AUD_INT_SYS_SEL(3));
}

static inline int arch_audio_codec_digital_reset(void)
{
	sci_glb_set(REG_AON_APB_APB_RST0, BIT_AUD_SOFT_RST);
	sci_glb_set(REG_AON_APB_APB_RST0, BIT_AUDIF_SOFT_RST);
	udelay(10);
	sci_glb_clr(REG_AON_APB_APB_RST0, BIT_AUD_SOFT_RST);
	sci_glb_clr(REG_AON_APB_APB_RST0, BIT_AUDIF_SOFT_RST);
	return 0;
}

static inline int arch_audio_codec_analog_reset(void)
{
	u16 mask = BIT_ANA_AUD_SOFT_RST | BIT_ANA_AUDTX_SOFT_RST |
		   BIT_ANA_AUDRX_SOFT_RST;
	int ret;

	ret = sci_adi_write(ANA_REG_GLB_ARM_RST, mask, mask);
	udelay(10);
	if (ret < 0)
		return ret;
	return sci_adi_write(ANA_REG_GLB_ARM_RST, 0, mask);
}

static inline int arch_audio_codec_reset(void)
{
	u16 mask = BIT_ANA_AUD_SOFT_RST | BIT_ANA_AUDTX_SOFT_RST |
		   BIT_ANA_AUDRX_SOFT_RST;
	int ret;

	sci_glb_set(REG_AON_APB_APB_RST0, BIT_AUD_SOFT_RST);
	sci_glb_set(REG_AON_APB_APB_RST0, BIT_AUDIF_SOFT_RST);
	ret = sci_adi_write(ANA_REG_GLB_ARM_RST, mask, mask);
	udelay(10);
	sci_glb_clr(REG_AON_APB_APB_RST0, BIT_AUD_SOFT_RST);
	sci_glb_clr(REG_AON_APB_APB_RST0, BIT_AUDIF_SOFT_RST);
	if (ret < 0)
		return ret;
	return sci_adi_write(ANA_REG_GLB_ARM_RST, 0, mask);
}

/* keep the 26 MHz crystal running in deep sleep while audio is active */
static inline int arch_audio_sleep_xtl_enable(void)
{
	return sci_glb_set(REG_PMU_APB_SLEEP_CTRL, BIT_AP_SLEEP_XTL_ON);
}

static inline int arch_audio_sleep_xtl_disable(void)
{
	return sci_glb_clr(REG_PMU_APB_SLEEP_CTRL, BIT_AP_SLEEP_XTL_ON);
}

/* only needed for digital FM, which this port doesn't support */
static inline int arch_audio_memory_sleep_enable(void)
{
	return 0;
}

static inline int arch_audio_memory_sleep_disable(void)
{
	return 0;
}

#endif /* __SOUND_SOC_SPRD_AUDIO_H */
