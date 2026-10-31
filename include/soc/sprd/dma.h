/*
 * Spreadtrum SC8830 / SC7730SE AP DMA controller (r4p0) interface
 *
 * This is the channel based interface of the vendor 3.10 mach-sc DMA
 * driver. The Spreadtrum ASoC PCM driver is written against it.
 *
 * Licensed under the GPL-2.
 */

#ifndef __SOC_SPRD_DMA_H
#define __SOC_SPRD_DMA_H

#include <linux/types.h>

/* Hardware request IDs (UIDs) */
#define DMA_UID_SOFTWARE	0
#define DMA_SIM_RX		1
#define DMA_SIM_TX		2
#define DMA_IIS0_RX		3
#define DMA_IIS0_TX		4
#define DMA_IIS1_RX		5
#define DMA_IIS1_TX		6
#define DMA_IIS2_RX		7
#define DMA_IIS2_TX		8
#define DMA_IIS3_RX		9
#define DMA_IIS3_TX		10
#define DMA_SPI0_RX		11
#define DMA_SPI0_TX		12
#define DMA_SPI1_RX		13
#define DMA_SPI1_TX		14
#define DMA_SPI2_RX		15
#define DMA_SPI2_TX		16
#define DMA_UART0_RX		17
#define DMA_UART0_TX		18
#define DMA_UART1_RX		19
#define DMA_UART1_TX		20
#define DMA_UART2_RX		21
#define DMA_UART2_TX		22
#define DMA_UART3_RX		23
#define DMA_UART3_TX		24
#define DMA_UART4_RX		25
#define DMA_UART4_TX		26
#define DMA_DRM_CPT		27
#define DMA_DRM_RAW		28
#define DMA_VB_DA0		29
#define DMA_VB_DA1		30
#define DMA_VB_AD0		31
#define DMA_VB_AD1		32
#define DMA_VB_AD2		33
#define DMA_VB_AD3		34
#define DMA_GPS			35
#define DMA_SDIO0_RD		36
#define DMA_SDIO0_WR		37
#define DMA_SDIO1_RD		38
#define DMA_SDIO1_WR		39
#define DMA_SDIO2_RD		40
#define DMA_SDIO2_WR		41
#define DMA_EMMC_RD		42
#define DMA_EMMC_WR		43

/* Channels are numbered from 1, the last 8 are "full" channels */
#define DMA_CHN_MIN		1
#define DMA_CHN_MAX		32
#define DMA_CHN_NUM		32
#define FULL_CHN_START		25
#define FULL_CHN_END		DMA_CHN_MAX

/* Size of one link list node (struct sci_dma_reg) */
#define DMA_LINKLIST_CFG_NODE_SIZE	64

typedef enum {
	NO_INT = 0x00,
	FRAG_DONE,
	BLK_DONE,
	LIST_DONE,
	TRANS_DONE,
	CONFIG_ERR,
} dma_int_type;

typedef enum {
	BYTE_WIDTH = 0x01,
	SHORT_WIDTH,
	WORD_WIDTH,
} dma_datawidth;

typedef enum {
	DMA_PRI_0 = 0,
	DMA_PRI_1,
	DMA_PRI_2,
	DMA_PRI_3,
} dma_pri_level;

typedef enum {
	STD_DMA_CHN,
	FULL_DMA_CHN,
} dma_chn_type;

typedef enum {
	FRAG_REQ_MODE = 0x0,
	BLOCK_REQ_MODE,
	TRANS_REQ_MODE,
	LIST_REQ_MODE,
} dma_request_mode;

/* CPU and bus address of the link list node array */
struct reg_cfg_addr {
	void *virt_addr;
	u32 phys_addr;
};

struct sci_dma_cfg {
	dma_datawidth datawidth;
	u32 src_addr;
	u32 des_addr;
	u32 fragmens_len;
	u32 block_len;
	u32 src_step;
	u32 des_step;
	dma_request_mode req_mode;
	/* only full channels use the following fields */
	u32 transcation_len;
	u32 src_frag_step;
	u32 dst_frag_step;
	u32 wrap_ptr;
	u32 wrap_to;
	u32 src_blk_step;
	u32 dst_blk_step;
	u32 linklist_ptr;
	u32 is_end;
};

int sci_dma_request(const char *dev_name, dma_chn_type chn_type);
int sci_dma_free(u32 dma_chn);
int sci_dma_config(u32 dma_chn, struct sci_dma_cfg *cfg_list,
		   u32 node_size, struct reg_cfg_addr *cfg_addr);
int sci_dma_register_irqhandle(u32 dma_chn, dma_int_type int_type,
			       void (*irq_handle)(int, void *), void *data);
int sci_dma_start(u32 dma_chn, u32 dev_id);
int sci_dma_stop(u32 dma_chn, u32 dev_id);
u32 sci_dma_get_src_addr(u32 dma_chn);
u32 sci_dma_get_dst_addr(u32 dma_chn);
int sci_dma_dump_reg(u32 dma_chn, void __iomem **reg_base);
int sci_dma_memcpy(u32 dest, u32 src, size_t size);

#endif /* __SOC_SPRD_DMA_H */
