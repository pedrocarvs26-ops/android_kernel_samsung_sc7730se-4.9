// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC7730SE (TShark) MM IOMMU, /dev/sprd_iommu_mm.
 *
 * The masters of the MM power domain (VSP, JPEG, DCAM) can address memory
 * through an I/O window that the IOMMU translates with a single-level
 * table in its own SRAM: one 32-bit entry, the physical address, per 4 KiB
 * page.  The vendor userspace only checks that the device node exists; the
 * OMX codecs then allocate from the ordinary ION heaps and get the I/O
 * addresses of their buffers from ION (ION_SPRD_CUSTOM_MM_MAP/MM_UNMAP),
 * which maps them here.  Without the node they fall back to contiguous
 * buffers with a gralloc usage bit that Android 11 refuses.
 *
 * As on the 3.10 vendor kernel without its light-sleep mode, the IOMMU is
 * switched on with the first mapping and off with the last.  clk_mm_i keeps
 * the MM domain up in between, so the table, rebuilt at each switch-on,
 * never needs saving.  Unmapped entries point at a fault page holding a
 * pattern that is checked at switch-off, to catch writes through stale
 * I/O addresses.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/sprd_iommu.h>

/* control register */
#define IOMMU_EN		BIT(0)
#define IOMMU_TLB_EN		BIT(1)
#define IOMMU_RAMCLK_DIV2_EN	BIT(2)
#define IOMMU_START_MB		GENMASK(31, 20)

/* unmapped entries: the fault page, low bits set as on 3.10 */
#define IOMMU_PTE_FAULT		0xfff
#define IOMMU_FAULT_PATTERN	0xdeaddead

struct sprd_iommu {
	struct device *dev;
	void __iomem *pgt;
	void __iomem *ctrl;
	unsigned long iova_base;
	size_t iova_size;
	struct clk *clk_mm;
	struct clk *clk_mmu;
	struct gen_pool *pool;
	struct page *fault_page;
	dma_addr_t fault_dma;
	u32 fault_pte;
	/* Serializes the table, the control register and map_count. */
	struct mutex lock;
	unsigned int map_count;
	struct miscdevice misc;
};

static struct sprd_iommu *sprd_mm_iommu;

static void sprd_iommu_ctrl(struct sprd_iommu *iommu, u32 mask, u32 val)
{
	u32 reg = readl(iommu->ctrl);

	/* writel() also orders the table updates before the change */
	writel((reg & ~mask) | (val & mask), iommu->ctrl);
}

static void __iomem *sprd_iommu_pte(struct sprd_iommu *iommu,
				    unsigned long iova)
{
	return iommu->pgt + ((iova - iommu->iova_base) >> PAGE_SHIFT) * 4;
}

static void sprd_iommu_clear(struct sprd_iommu *iommu, unsigned long iova,
			     size_t size)
{
	void __iomem *pte = sprd_iommu_pte(iommu, iova);
	size_t i;

	for (i = 0; i < size >> PAGE_SHIFT; i++)
		writel_relaxed(iommu->fault_pte, pte + 4 * i);
}

static int sprd_iommu_hw_on(struct sprd_iommu *iommu)
{
	int ret;

	ret = clk_prepare_enable(iommu->clk_mm);
	if (ret)
		return ret;
	ret = clk_prepare_enable(iommu->clk_mmu);
	if (ret) {
		clk_disable_unprepare(iommu->clk_mm);
		return ret;
	}

	/* the MM domain may have been down: start from an empty table */
	sprd_iommu_ctrl(iommu, IOMMU_EN, 0);
	sprd_iommu_clear(iommu, iommu->iova_base, iommu->iova_size);
	sprd_iommu_ctrl(iommu, IOMMU_RAMCLK_DIV2_EN, IOMMU_RAMCLK_DIV2_EN);
	sprd_iommu_ctrl(iommu, IOMMU_START_MB, iommu->iova_base);
	sprd_iommu_ctrl(iommu, IOMMU_TLB_EN, IOMMU_TLB_EN);
	sprd_iommu_ctrl(iommu, IOMMU_EN, IOMMU_EN);
	return 0;
}

static void sprd_iommu_hw_off(struct sprd_iommu *iommu)
{
	u32 *fault = page_address(iommu->fault_page);
	bool tainted = false;
	unsigned int i;

	sprd_iommu_ctrl(iommu, IOMMU_RAMCLK_DIV2_EN, 0);
	sprd_iommu_ctrl(iommu, IOMMU_TLB_EN, 0);
	sprd_iommu_ctrl(iommu, IOMMU_EN, 0);
	clk_disable_unprepare(iommu->clk_mmu);
	clk_disable_unprepare(iommu->clk_mm);

	dma_sync_single_for_cpu(iommu->dev, iommu->fault_dma, PAGE_SIZE,
				DMA_BIDIRECTIONAL);
	for (i = 0; i < PAGE_SIZE / sizeof(*fault); i++) {
		if (fault[i] != IOMMU_FAULT_PATTERN) {
			fault[i] = IOMMU_FAULT_PATTERN;
			tainted = true;
		}
	}
	dma_sync_single_for_device(iommu->dev, iommu->fault_dma, PAGE_SIZE,
				   DMA_BIDIRECTIONAL);
	if (tainted)
		dev_warn(iommu->dev, "write through an unmapped I/O address\n");
}

/**
 * sprd_iommu_mm_map - map pages into the I/O window of the MM masters
 * @table: the pages, as whole pages without offsets
 * @size: number of bytes to map from the start of @table
 * @iova: returns the I/O address
 *
 * Cache maintenance is left to the caller.
 *
 * Return: 0, -ENODEV without the IOMMU, -EINVAL for an unusable @table or
 * -ENOMEM when the window is full.
 */
int sprd_iommu_mm_map(struct sg_table *table, size_t size,
		      unsigned long *iova)
{
	/* pairs with the release in probe */
	struct sprd_iommu *iommu = smp_load_acquire(&sprd_mm_iommu);
	struct scatterlist *sg;
	unsigned long addr, cur, end;
	size_t len = 0;
	unsigned int i;
	int ret;

	if (!iommu)
		return -ENODEV;
	size = PAGE_ALIGN(size);
	if (!size)
		return -EINVAL;
	for_each_sg(table->sgl, sg, table->nents, i) {
		if (sg->offset || !PAGE_ALIGNED(sg->length))
			return -EINVAL;
		len += sg->length;
	}
	if (len < size)
		return -EINVAL;

	addr = gen_pool_alloc(iommu->pool, size);
	if (!addr)
		return -ENOMEM;

	mutex_lock(&iommu->lock);
	if (!iommu->map_count) {
		ret = sprd_iommu_hw_on(iommu);
		if (ret) {
			mutex_unlock(&iommu->lock);
			gen_pool_free(iommu->pool, addr, size);
			return ret;
		}
	}
	iommu->map_count++;

	sprd_iommu_ctrl(iommu, IOMMU_TLB_EN, 0);
	cur = addr;
	end = addr + size;
	for_each_sg(table->sgl, sg, table->nents, i) {
		phys_addr_t pa = sg_phys(sg);
		unsigned int n;

		for (n = 0; n < sg->length && cur < end; n += PAGE_SIZE) {
			writel_relaxed(pa + n, sprd_iommu_pte(iommu, cur));
			cur += PAGE_SIZE;
		}
		if (cur == end)
			break;
	}
	sprd_iommu_ctrl(iommu, IOMMU_TLB_EN, IOMMU_TLB_EN);
	mutex_unlock(&iommu->lock);

	*iova = addr;
	return 0;
}

/**
 * sprd_iommu_mm_unmap - undo sprd_iommu_mm_map()
 * @iova: the I/O address it returned
 * @size: the size it was given
 */
void sprd_iommu_mm_unmap(unsigned long iova, size_t size)
{
	/* pairs with the release in probe */
	struct sprd_iommu *iommu = smp_load_acquire(&sprd_mm_iommu);

	size = PAGE_ALIGN(size);
	if (WARN_ON(!iommu || !size || size > iommu->iova_size ||
		    iova < iommu->iova_base ||
		    iova - iommu->iova_base > iommu->iova_size - size))
		return;

	mutex_lock(&iommu->lock);
	if (WARN_ON(!iommu->map_count)) {
		mutex_unlock(&iommu->lock);
		return;
	}
	sprd_iommu_ctrl(iommu, IOMMU_TLB_EN, 0);
	sprd_iommu_clear(iommu, iova, size);
	sprd_iommu_ctrl(iommu, IOMMU_TLB_EN, IOMMU_TLB_EN);
	if (!--iommu->map_count)
		sprd_iommu_hw_off(iommu);
	mutex_unlock(&iommu->lock);

	gen_pool_free(iommu->pool, iova, size);
}

static const struct file_operations sprd_iommu_fops = {
	.owner		= THIS_MODULE,
};

static int sprd_iommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprd_iommu *iommu;
	struct resource *res;
	unsigned int i;
	u32 base, *fault;
	int ret;

	if (sprd_mm_iommu)
		return -EBUSY;

	iommu = devm_kzalloc(dev, sizeof(*iommu), GFP_KERNEL);
	if (!iommu)
		return -ENOMEM;
	iommu->dev = dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pgt");
	iommu->pgt = devm_ioremap_resource(dev, res);
	if (IS_ERR(iommu->pgt))
		return PTR_ERR(iommu->pgt);
	iommu->iova_size = resource_size(res) / 4 * PAGE_SIZE;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ctrl");
	iommu->ctrl = devm_ioremap_resource(dev, res);
	if (IS_ERR(iommu->ctrl))
		return PTR_ERR(iommu->ctrl);

	ret = of_property_read_u32(dev->of_node, "sprd,iova-base", &base);
	if (ret)
		return ret;
	/* START_MB is in MiB; keep the window naturally aligned as on 3.10 */
	if (!base || iommu->iova_size < SZ_1M ||
	    !is_power_of_2(iommu->iova_size) ||
	    base & (iommu->iova_size - 1)) {
		dev_err(dev, "bad window %#x + %#zx\n", base, iommu->iova_size);
		return -EINVAL;
	}
	iommu->iova_base = base;

	iommu->clk_mm = devm_clk_get(dev, "clk_mm_i");
	if (IS_ERR(iommu->clk_mm))
		return PTR_ERR(iommu->clk_mm);
	iommu->clk_mmu = devm_clk_get(dev, "clk_mmu");
	if (IS_ERR(iommu->clk_mmu))
		return PTR_ERR(iommu->clk_mmu);

	iommu->pool = devm_gen_pool_create(dev, PAGE_SHIFT, -1, NULL);
	if (IS_ERR(iommu->pool))
		return PTR_ERR(iommu->pool);
	ret = gen_pool_add(iommu->pool, iommu->iova_base, iommu->iova_size,
			   -1);
	if (ret)
		return ret;

	iommu->fault_page = alloc_page(GFP_KERNEL);
	if (!iommu->fault_page)
		return -ENOMEM;
	fault = page_address(iommu->fault_page);
	for (i = 0; i < PAGE_SIZE / sizeof(*fault); i++)
		fault[i] = IOMMU_FAULT_PATTERN;
	iommu->fault_dma = dma_map_page(dev, iommu->fault_page, 0, PAGE_SIZE,
					DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, iommu->fault_dma)) {
		ret = -ENOMEM;
		goto err_page;
	}
	iommu->fault_pte = page_to_phys(iommu->fault_page) | IOMMU_PTE_FAULT;

	mutex_init(&iommu->lock);
	iommu->misc.minor = MISC_DYNAMIC_MINOR;
	iommu->misc.name = "sprd_iommu_mm";
	iommu->misc.fops = &sprd_iommu_fops;
	iommu->misc.parent = dev;

	/* ready before the node userspace looks for appears */
	smp_store_release(&sprd_mm_iommu, iommu);
	ret = misc_register(&iommu->misc);
	if (ret) {
		WRITE_ONCE(sprd_mm_iommu, NULL);
		goto err_unmap;
	}

	dev_info(dev, "window %#lx + %zu MiB\n", iommu->iova_base,
		 iommu->iova_size >> 20);
	return 0;

err_unmap:
	dma_unmap_page(dev, iommu->fault_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
err_page:
	__free_page(iommu->fault_page);
	return ret;
}

static const struct of_device_id sprd_iommu_of_match[] = {
	{ .compatible = "sprd,scx30g-mm-iommu" },
	{ }
};

static struct platform_driver sprd_iommu_driver = {
	.probe = sprd_iommu_probe,
	.driver = {
		.name = "sprd-iommu-mm",
		.of_match_table = sprd_iommu_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(sprd_iommu_driver);
