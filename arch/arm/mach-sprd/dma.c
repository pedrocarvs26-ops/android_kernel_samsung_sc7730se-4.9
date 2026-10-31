/*
 * Spreadtrum SC8830 / SC7730SE AP DMA controller (r4p0)
 *
 * Port of arch/arm/mach-sc/dma_r4p0.c from the vendor 3.10 kernel. It
 * keeps the vendor channel based sci_dma_* interface that the
 * Spreadtrum ASoC PCM driver uses.
 *
 * Licensed under the GPL-2.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <soc/sprd/dma.h>
#include <soc/sprd/iomap.h>

/* global registers */
#define DMA_FRAG_WAIT		0x0004
#define DMA_INT_MSK_STS		0x0014

/* channel registers, channels are numbered from 1 */
#define DMA_CHN_OFFSET		0x40
#define DMA_CHN_BASE(ch)	(0x1000 + DMA_CHN_OFFSET * ((ch) - 1))
#define DMA_CHN_PAUSE(ch)	(DMA_CHN_BASE(ch) + 0x00)
#define DMA_CHN_REQ(ch)		(DMA_CHN_BASE(ch) + 0x04)
#define DMA_CHN_CFG(ch)		(DMA_CHN_BASE(ch) + 0x08)
#define DMA_CHN_INT(ch)		(DMA_CHN_BASE(ch) + 0x0c)
#define DMA_CHN_SRC_ADR(ch)	(DMA_CHN_BASE(ch) + 0x10)
#define DMA_CHN_DES_ADR(ch)	(DMA_CHN_BASE(ch) + 0x14)

/* request ID to channel map, request IDs are numbered from 1 */
#define DMA_REQ_CID(uid)	(0x2000 + 0x4 * ((uid) - 1))

#define CHN_EN			BIT(0)
#define CHN_LLIST_EN_SHIFT	4
#define CHN_PRIORITY_SHIFT	12
#define CHN_PAUSE		BIT(0)
#define CHN_PAUSE_DONE		BIT(16)
#define CHN_INT_TYPE_MASK	0x1f
#define CHN_INT_CLR_ALL		(0x1f << 24)
#define CHN_SOFT_REQ		BIT(0)

/* frg_len */
#define SRC_DATAWIDTH_SHIFT	30
#define DES_DATAWIDTH_SHIFT	28
#define SWT_MODE_SHIFT		26
#define REQ_MODE_SHIFT		24
#define ADDR_WRAP_SEL_SHIFT	23
#define ADDR_WRAP_EN_SHIFT	22
#define ADDR_FIX_SEL_SHIFT	21
#define ADDR_FIX_EN_SHIFT	20
#define LLIST_END_SHIFT		19
#define FRG_LEN_MASK		0x1ffff
#define BLK_LEN_MASK		0x1ffff
#define TRSC_LEN_MASK		0xfffffff
#define TRSF_STEP_MASK		0xffff
#define FRAG_STEP_MASK		0xffff
#define DEST_STEP_SHIFT		16

/* channel register block, also the layout of a link list node */
struct sci_dma_reg {
	u32 pause;
	u32 req;
	u32 cfg;
	u32 intc;
	u32 src_addr;
	u32 des_addr;
	u32 frg_len;
	u32 blk_len;
	u32 trsc_len;
	u32 trsf_step;
	u32 wrap_ptr;
	u32 wrap_to;
	u32 llist_ptr;
	u32 frg_step;
	u32 src_blk_step;
	u32 des_blk_step;
};

/* the hardware loads a node starting at its src_addr field */
#define DMA_LLIST_LOAD_OFFSET	offsetof(struct sci_dma_reg, src_addr)

struct sci_dma_desc {
	const char *dev_name;
	void (*irq_handler)(int chn, void *data);
	void *data;
	u32 dev_id;
};

static void __iomem *dma_base;
static struct sci_dma_desc dma_chns[DMA_CHN_NUM + 1];
static DEFINE_SPINLOCK(dma_lock);
static DEFINE_MUTEX(dma_mutex);

static inline bool dma_chn_invalid(u32 chn)
{
	return !dma_base || chn < DMA_CHN_MIN || chn > DMA_CHN_MAX;
}

static void dma_clk_enable(void)
{
	if (!sci_glb_read(REG_AP_AHB_AHB_EB, BIT_DMA_EB))
		sci_glb_set(REG_AP_AHB_AHB_EB, BIT_DMA_EB);
}

static void dma_clk_disable(void)
{
	sci_glb_clr(REG_AP_AHB_AHB_EB, BIT_DMA_EB);
}

static int dma_set_int_type(u32 chn, dma_int_type int_type)
{
	u32 val = readl_relaxed(dma_base + DMA_CHN_INT(chn));

	val &= ~CHN_INT_TYPE_MASK;
	switch (int_type) {
	case NO_INT:
		break;
	case FRAG_DONE:
		val |= 0x1;
		break;
	case BLK_DONE:
		val |= 0x2;
		break;
	case TRANS_DONE:
		val |= 0x4;
		break;
	case LIST_DONE:
		val |= 0x8;
		break;
	case CONFIG_ERR:
		val |= 0x10;
		break;
	default:
		return -EINVAL;
	}
	writel_relaxed(val, dma_base + DMA_CHN_INT(chn));

	return 0;
}

/* translate a struct sci_dma_cfg into a register block */
static int dma_cfg_convert(u32 chn, const struct sci_dma_cfg *cfg,
			   struct sci_dma_reg *reg)
{
	u32 datawidth = 0, list_end = 0, fix_en = 0, fix_mode = 0;
	u32 llist_en = 0, wrap_en = 0, wrap_mode = 0;

	/*
	 * Only full channels support transaction transfers, link lists,
	 * fifo to fifo copies, the wrap mode and fragment or block steps.
	 */
	if ((cfg->transcation_len || cfg->linklist_ptr ||
	     (cfg->src_step | cfg->des_step) == 0 ||
	     (cfg->wrap_ptr && cfg->wrap_to) ||
	     (cfg->src_frag_step | cfg->dst_frag_step) ||
	     (cfg->src_blk_step | cfg->dst_blk_step)) &&
	    chn < FULL_CHN_START)
		return -EINVAL;

	switch (cfg->datawidth) {
	case BYTE_WIDTH:
		datawidth = 0;
		break;
	case SHORT_WIDTH:
		datawidth = 1;
		break;
	case WORD_WIDTH:
		datawidth = 2;
		break;
	default:
		if (cfg->linklist_ptr)
			break;
		pr_err("sprd-dma: bad data width %d\n", cfg->datawidth);
		return -EINVAL;
	}

	if (cfg->datawidth && (!IS_ALIGNED(cfg->src_step, cfg->datawidth) ||
			       !IS_ALIGNED(cfg->des_step, cfg->datawidth)))
		return -EINVAL;

	if ((cfg->src_step == 0) != (cfg->des_step == 0)) {
		fix_en = 1;
		fix_mode = cfg->src_step ? 1 : 0;
	}

	if (cfg->wrap_ptr && cfg->wrap_to) {
		wrap_en = 1;
		if (cfg->wrap_to == cfg->src_addr)
			wrap_mode = 0;
		else if (cfg->wrap_to == cfg->des_addr)
			wrap_mode = 1;
		else
			return -EINVAL;
	}

	if (cfg->linklist_ptr) {
		llist_en = 1;
		if (cfg->is_end)
			list_end = 1;
	}

	memset(reg, 0, sizeof(*reg));
	reg->cfg = DMA_PRI_1 << CHN_PRIORITY_SHIFT |
		   llist_en << CHN_LLIST_EN_SHIFT;
	reg->src_addr = cfg->src_addr;
	reg->des_addr = cfg->des_addr;
	reg->frg_len = datawidth << SRC_DATAWIDTH_SHIFT |
		       datawidth << DES_DATAWIDTH_SHIFT |
		       0 << SWT_MODE_SHIFT |
		       cfg->req_mode << REQ_MODE_SHIFT |
		       wrap_mode << ADDR_WRAP_SEL_SHIFT |
		       wrap_en << ADDR_WRAP_EN_SHIFT |
		       fix_mode << ADDR_FIX_SEL_SHIFT |
		       fix_en << ADDR_FIX_EN_SHIFT |
		       list_end << LLIST_END_SHIFT |
		       (cfg->fragmens_len & FRG_LEN_MASK);
	reg->blk_len = cfg->block_len & BLK_LEN_MASK;

	if (chn < FULL_CHN_START)
		return 0;

	if (cfg->transcation_len)
		reg->trsc_len = cfg->transcation_len & TRSC_LEN_MASK;
	else
		reg->trsc_len = cfg->block_len & TRSC_LEN_MASK;
	reg->trsf_step = (cfg->des_step & TRSF_STEP_MASK) << DEST_STEP_SHIFT |
			 (cfg->src_step & TRSF_STEP_MASK);
	reg->wrap_ptr = cfg->wrap_ptr;
	reg->wrap_to = cfg->wrap_to;
	reg->llist_ptr = cfg->linklist_ptr;
	reg->frg_step = (cfg->dst_frag_step & FRAG_STEP_MASK) <<
			DEST_STEP_SHIFT |
			(cfg->src_frag_step & FRAG_STEP_MASK);
	reg->src_blk_step = cfg->src_blk_step;
	reg->des_blk_step = cfg->dst_blk_step;

	return 0;
}

/* program the channel registers, the interrupt setup is left alone */
static void dma_write_chn(u32 chn, const struct sci_dma_reg *reg)
{
	void __iomem *base = dma_base + DMA_CHN_BASE(chn);

	writel_relaxed(reg->pause, base + 0x00);
	writel_relaxed(reg->req, base + 0x04);
	writel_relaxed(reg->cfg, base + 0x08);
	writel_relaxed(reg->src_addr, base + 0x10);
	writel_relaxed(reg->des_addr, base + 0x14);
	writel_relaxed(reg->frg_len, base + 0x18);
	writel_relaxed(reg->blk_len, base + 0x1c);
	if (chn < FULL_CHN_START)
		return;
	writel_relaxed(reg->trsc_len, base + 0x20);
	writel_relaxed(reg->trsf_step, base + 0x24);
	writel_relaxed(reg->wrap_ptr, base + 0x28);
	writel_relaxed(reg->wrap_to, base + 0x2c);
	writel_relaxed(reg->llist_ptr, base + 0x30);
	writel_relaxed(reg->frg_step, base + 0x34);
	writel_relaxed(reg->src_blk_step, base + 0x38);
	writel_relaxed(reg->des_blk_step, base + 0x3c);
}

static void dma_int_clr(u32 chn)
{
	u32 val = readl_relaxed(dma_base + DMA_CHN_INT(chn));

	writel_relaxed(val | CHN_INT_CLR_ALL, dma_base + DMA_CHN_INT(chn));
}

static void dma_int_dis(u32 chn)
{
	writel_relaxed(CHN_INT_CLR_ALL, dma_base + DMA_CHN_INT(chn));
}

static void dma_set_uid(u32 chn, u32 dev_id)
{
	if (dev_id != DMA_UID_SOFTWARE)
		writel_relaxed(chn, dma_base + DMA_REQ_CID(dev_id));
}

static void dma_stop_and_disable(u32 chn)
{
	void __iomem *cfg = dma_base + DMA_CHN_CFG(chn);
	void __iomem *pause = dma_base + DMA_CHN_PAUSE(chn);
	int timeout = 1000;

	if (!(readl_relaxed(cfg) & CHN_EN))
		return;

	writel_relaxed(readl_relaxed(pause) | CHN_PAUSE, pause);
	while (!(readl_relaxed(pause) & CHN_PAUSE_DONE) && --timeout)
		udelay(1);
	if (!timeout)
		pr_warn("sprd-dma: channel %u pause timeout\n", chn);

	writel_relaxed(readl_relaxed(cfg) & ~CHN_EN, cfg);
	writel_relaxed(readl_relaxed(pause) & ~CHN_PAUSE, pause);
}

static irqreturn_t sci_dma_irq(int irq, void *dev_id)
{
	u32 status;
	int i;

	spin_lock(&dma_lock);
	status = readl_relaxed(dma_base + DMA_INT_MSK_STS);
	if (!status) {
		spin_unlock(&dma_lock);
		return IRQ_NONE;
	}

	while (status) {
		i = __ffs(status) + 1;
		status &= status - 1;
		dma_int_clr(i);
		if (dma_chns[i].irq_handler)
			dma_chns[i].irq_handler(i, dma_chns[i].data);
	}
	spin_unlock(&dma_lock);

	return IRQ_HANDLED;
}

int sci_dma_start(u32 dma_chn, u32 dev_id)
{
	void __iomem *cfg;

	if (dma_chn_invalid(dma_chn))
		return -EINVAL;

	cfg = dma_base + DMA_CHN_CFG(dma_chn);
	dma_chns[dma_chn].dev_id = dev_id;
	dma_set_uid(dma_chn, dev_id);
	/* make the link list nodes visible before the channel runs */
	wmb();
	writel_relaxed(readl_relaxed(cfg) | CHN_EN, cfg);
	if (dev_id == DMA_UID_SOFTWARE)
		writel_relaxed(readl_relaxed(dma_base + DMA_CHN_REQ(dma_chn)) |
			       CHN_SOFT_REQ, dma_base + DMA_CHN_REQ(dma_chn));

	return 0;
}
EXPORT_SYMBOL_GPL(sci_dma_start);

int sci_dma_stop(u32 dma_chn, u32 dev_id)
{
	if (dma_chn_invalid(dma_chn))
		return -EINVAL;

	dma_stop_and_disable(dma_chn);
	dma_int_clr(dma_chn);

	return 0;
}
EXPORT_SYMBOL_GPL(sci_dma_stop);

int sci_dma_register_irqhandle(u32 dma_chn, dma_int_type int_type,
			       void (*irq_handle)(int, void *), void *data)
{
	unsigned long flags;
	int ret;

	if (dma_chn_invalid(dma_chn) || !irq_handle)
		return -EINVAL;

	spin_lock_irqsave(&dma_lock, flags);
	ret = dma_set_int_type(dma_chn, int_type);
	if (!ret) {
		dma_chns[dma_chn].irq_handler = irq_handle;
		dma_chns[dma_chn].data = data;
	}
	spin_unlock_irqrestore(&dma_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(sci_dma_register_irqhandle);

int sci_dma_config(u32 dma_chn, struct sci_dma_cfg *cfg_list,
		   u32 node_size, struct reg_cfg_addr *cfg_addr)
{
	struct sci_dma_reg *nodes, reg;
	struct sci_dma_cfg list_cfg;
	int ret;
	u32 i;

	if (dma_chn_invalid(dma_chn))
		return -EINVAL;

	dma_clk_enable();

	if (node_size <= 1) {
		ret = dma_cfg_convert(dma_chn, cfg_list, &reg);
		if (ret)
			return ret;
		dma_write_chn(dma_chn, &reg);
		return 0;
	}

	if (!cfg_addr || !cfg_addr->virt_addr)
		return -EINVAL;

	/* build a ring of nodes, each one pointing to the next one */
	nodes = cfg_addr->virt_addr;
	for (i = 0; i < node_size; i++) {
		cfg_list[i].linklist_ptr = cfg_addr->phys_addr +
			((i + 1) % node_size) * sizeof(struct sci_dma_reg) +
			DMA_LLIST_LOAD_OFFSET;
		ret = dma_cfg_convert(dma_chn, &cfg_list[i], &nodes[i]);
		if (ret)
			return ret;
	}

	/* the channel itself only points to the first node */
	memset(&list_cfg, 0, sizeof(list_cfg));
	list_cfg.linklist_ptr = cfg_addr->phys_addr + DMA_LLIST_LOAD_OFFSET;
	/* the PCM driver reads the first addresses back from the channel */
	list_cfg.src_addr = cfg_list[0].src_addr;
	list_cfg.des_addr = cfg_list[0].des_addr;
	ret = dma_cfg_convert(dma_chn, &list_cfg, &reg);
	if (ret)
		return ret;
	dma_write_chn(dma_chn, &reg);

	return 0;
}
EXPORT_SYMBOL_GPL(sci_dma_config);

int sci_dma_request(const char *dev_name, dma_chn_type chn_type)
{
	u32 start, end, i;
	int chn = -EBUSY;

	if (!dev_name)
		return -EINVAL;
	if (!dma_base)
		return -ENODEV;

	if (chn_type == FULL_DMA_CHN) {
		start = FULL_CHN_START;
		end = FULL_CHN_END;
	} else {
		start = DMA_CHN_MIN;
		end = DMA_CHN_MAX;
	}

	mutex_lock(&dma_mutex);
	for (i = start; i <= end; i++) {
		if (!dma_chns[i].dev_name) {
			dma_chns[i].dev_name = dev_name;
			chn = i;
			break;
		}
	}
	mutex_unlock(&dma_mutex);

	return chn;
}
EXPORT_SYMBOL_GPL(sci_dma_request);

int sci_dma_free(u32 dma_chn)
{
	unsigned long flags;
	u32 i;

	if (dma_chn_invalid(dma_chn))
		return -EINVAL;

	dma_stop_and_disable(dma_chn);
	dma_int_dis(dma_chn);
	/* drop the request ID mapping, channel 0 does not exist */
	dma_set_uid(0, dma_chns[dma_chn].dev_id);

	mutex_lock(&dma_mutex);
	spin_lock_irqsave(&dma_lock, flags);
	memset(&dma_chns[dma_chn], 0, sizeof(dma_chns[dma_chn]));
	spin_unlock_irqrestore(&dma_lock, flags);

	/* gate the controller clock once no channel is left */
	for (i = DMA_CHN_MIN; i <= DMA_CHN_MAX; i++)
		if (dma_chns[i].dev_name)
			break;
	if (i > DMA_CHN_MAX)
		dma_clk_disable();
	mutex_unlock(&dma_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(sci_dma_free);

/* current addresses, the audio driver uses them as position */
u32 sci_dma_get_src_addr(u32 dma_chn)
{
	if (dma_chn_invalid(dma_chn))
		return 0;

	return readl_relaxed(dma_base + DMA_CHN_SRC_ADR(dma_chn));
}
EXPORT_SYMBOL_GPL(sci_dma_get_src_addr);

u32 sci_dma_get_dst_addr(u32 dma_chn)
{
	if (dma_chn_invalid(dma_chn))
		return 0;

	return readl_relaxed(dma_base + DMA_CHN_DES_ADR(dma_chn));
}
EXPORT_SYMBOL_GPL(sci_dma_get_dst_addr);

/* returns the size of the channel register block */
int sci_dma_dump_reg(u32 dma_chn, void __iomem **reg_base)
{
	if (dma_chn_invalid(dma_chn))
		return -EINVAL;

	if (reg_base)
		*reg_base = dma_base + DMA_CHN_BASE(dma_chn);

	return DMA_CHN_OFFSET;
}
EXPORT_SYMBOL_GPL(sci_dma_dump_reg);

#define DMA_MEMCPY_MIN_SIZE 64
#define DMA_MEMCPY_MAX_SIZE TRSC_LEN_MASK

static void sci_dma_memcpy_irqhandle(int chn, void *data)
{
	struct semaphore *dma_sema = (struct semaphore *)data;
	up(dma_sema);
}

int sci_dma_memcpy(u32 dest, u32 src, size_t size)
{
	int ret;
	int dma_chn;
	u32 data_width, src_step;
	u32 irq_mode;
	struct sci_dma_cfg cfg;
	struct semaphore dma_sema;

	if (size < DMA_MEMCPY_MIN_SIZE || size > DMA_MEMCPY_MAX_SIZE)
		return -EINVAL;

	if (size <= BLK_LEN_MASK)
		dma_chn = sci_dma_request("dma memcpy", STD_DMA_CHN);
	else
		dma_chn = sci_dma_request("dma memcpy", FULL_DMA_CHN);

	if (dma_chn < 0) {
		pr_err("alloc dma chn fail\n");
		return dma_chn;
	}

	sema_init(&dma_sema, 0);
	memset(&cfg, 0x0, sizeof(cfg));

	if ((size & 0x3) == 0) {
		data_width = WORD_WIDTH;
		src_step = 4;
	} else if ((size & 0x1) == 0) {
		data_width = SHORT_WIDTH;
		src_step = 2;
	} else {
		data_width = BYTE_WIDTH;
		src_step = 1;
	}

	cfg.src_addr = src;
	cfg.des_addr = dest;
	cfg.datawidth = data_width;
	cfg.src_step = src_step;
	cfg.des_step = src_step;
	cfg.fragmens_len = DMA_MEMCPY_MIN_SIZE;

	if (size <= BLK_LEN_MASK) {
		cfg.block_len = size;
		cfg.req_mode = BLOCK_REQ_MODE;
		irq_mode = BLK_DONE;
	} else {
		cfg.block_len = DMA_MEMCPY_MIN_SIZE;
		cfg.transcation_len = size;
		cfg.req_mode = TRANS_REQ_MODE;
		irq_mode = TRANS_DONE;
	}

	ret = sci_dma_config(dma_chn, &cfg, 1, NULL);
	if (ret < 0) {
		pr_err("dma memcpy config error!\n");
		sci_dma_free(dma_chn);
		return -EINVAL;
	}

	ret = sci_dma_register_irqhandle(dma_chn, irq_mode,
					 sci_dma_memcpy_irqhandle,
					 &dma_sema);
	if (ret < 0) {
		pr_err("dma register irqhandle failed!\n");
		sci_dma_free(dma_chn);
		return ret;
	}

	sci_dma_start(dma_chn, DMA_UID_SOFTWARE);
	down(&dma_sema);
	sci_dma_free(dma_chn);

	return 0;
}
EXPORT_SYMBOL_GPL(sci_dma_memcpy);

static int sci_dma_probe(struct platform_device *pdev)
{
	struct resource *res;
	void __iomem *base;
	int irq, ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dma_clk_enable();
	writel_relaxed(0, base + DMA_FRAG_WAIT);
	dma_clk_disable();

	ret = devm_request_irq(&pdev->dev, irq, sci_dma_irq, 0, "sci-dma",
			       NULL);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq %d: %d\n", irq, ret);
		return ret;
	}

	dma_base = base;
	dev_info(&pdev->dev, "AP DMA ready, irq %d\n", irq);

	return 0;
}

static const struct of_device_id sci_dma_of_match[] = {
	{ .compatible = "sprd,sc8830-dma", },
	{ .compatible = "sprd,sprd-dma", },
	{ }
};

static struct platform_driver sci_dma_driver = {
	.probe = sci_dma_probe,
	.driver = {
		.name = "sprd-dma",
		.of_match_table = sci_dma_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init sci_dma_init(void)
{
	return platform_driver_register(&sci_dma_driver);
}
arch_initcall(sci_dma_init);
