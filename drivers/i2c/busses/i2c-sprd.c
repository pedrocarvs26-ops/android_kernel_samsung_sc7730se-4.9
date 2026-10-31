/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) I2C Master Bus Driver
 *
 * Adapted from drivers/i2c/busses/i2c-sprd.c in the 3.10 kernel for Linux 4.9.337.
 * Supports hardware I2C controllers i2c0..i2c4 at 0x70500000..0x70900000.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>

#include <soc/sprd/iomap.h>

#define SPRD_I2C_CTL_ID		6

#define I2C_CTL			0x0000
#define I2C_CMD			0x0004
#define I2C_CLKD0		0x0008
#define I2C_CLKD1		0x000C
#define I2C_RST			0x0010
#define I2C_CMD_BUF		0x0014
#define I2C_CMD_BUF_CTL		0x0018

#define I2C_CTL_INT		BIT(0)
#define I2C_CTL_ACK		BIT(1)
#define I2C_CTL_BUSY		BIT(2)
#define I2C_CTL_IE		BIT(3)
#define I2C_CTL_EN		BIT(4)
#define I2C_CTL_SCL_LINE	BIT(5)
#define I2C_CTL_SDA_LINE	BIT(6)
#define I2C_CTL_NOACK_INT_EN	BIT(7)
#define I2C_CTL_NOACK_INT_STS	BIT(8)
#define I2C_CTL_NOACK_INT_CLR	BIT(9)

#define I2C_CMD_INT_ACK		BIT(0)
#define I2C_CMD_TX_ACK		BIT(1)
#define I2C_CMD_WRITE		BIT(2)
#define I2C_CMD_READ		BIT(3)
#define I2C_CMD_STOP		BIT(4)
#define I2C_CMD_START		BIT(5)
#define I2C_CMD_ACK		BIT(6)
#define I2C_CMD_BUSY		BIT(7)
#define I2C_CMD_DATA		0xFF00

#define I2C_RST_RST		BIT(0)
#define I2C_CTL_CMDBUF_EN	BIT(0)
#define I2C_CTL_CMDBUF_EXEC	BIT(1)

struct sprd_i2c {
	struct i2c_msg *msg;
	struct i2c_adapter adap;
	void __iomem *membase;
	struct clk *clk;
	int irq;
	unsigned int bus_freq;
};

static struct sprd_i2c *sprd_i2c_ctl_id[SPRD_I2C_CTL_ID];

static void sprd_i2c_set_clk(struct sprd_i2c *pi2c, unsigned int freq);

static void sprd_i2c_hw_clk_enable(struct sprd_i2c *pi2c, bool enable)
{
	int id = pi2c->adap.nr;

	if (!IS_ERR_OR_NULL(pi2c->clk)) {
		if (enable)
			clk_prepare_enable(pi2c->clk);
		else
			clk_disable_unprepare(pi2c->clk);
	}

	if (enable && id >= 0 && id <= 4) {
		sci_glb_set(REG_AON_APB_APB_EB0, BIT(31)); /* clk_i2c gate */
		sci_glb_set(REG_AP_APB_APB_EB, BIT_I2C0_EB << id);
	}
}

static inline int sprd_i2c_poll_ctl_status(struct sprd_i2c *pi2c, unsigned long bit)
{
	int loop_cntr = 5000;

	do {
		udelay(1);
	} while (!(__raw_readl(pi2c->membase + I2C_CTL) & bit) && (--loop_cntr > 0));

	return (loop_cntr > 0) ? 1 : -ETIMEDOUT;
}

static inline int sprd_i2c_poll_cmd_status(struct sprd_i2c *pi2c, unsigned long bit)
{
	int loop_cntr = 5000;

	do {
		udelay(1);
	} while ((__raw_readl(pi2c->membase + I2C_CMD) & bit) && (--loop_cntr > 0));

	return (loop_cntr > 0) ? 1 : -ETIMEDOUT;
}

static inline void sprd_i2c_clear_int(struct sprd_i2c *pi2c)
{
	unsigned int cmd;

	sprd_i2c_poll_cmd_status(pi2c, I2C_CMD_BUSY);
	cmd = (__raw_readl(pi2c->membase + I2C_CMD) & 0xff00) | I2C_CMD_INT_ACK;
	__raw_writel(cmd, pi2c->membase + I2C_CMD);
}

static inline int sprd_wait_trx_done(struct sprd_i2c *pi2c)
{
	int rc;

	rc = sprd_i2c_poll_ctl_status(pi2c, I2C_CTL_INT);
	if (rc < 0)
		return rc;

	sprd_i2c_clear_int(pi2c);
	return sprd_i2c_poll_cmd_status(pi2c, I2C_CMD_ACK);
}

static int sprd_i2c_write_byte(struct sprd_i2c *pi2c, u8 byte, int stop, int is_last_msg)
{
	u32 cmd;

	if (stop && is_last_msg)
		cmd = ((u32)byte << 8) | I2C_CMD_WRITE | I2C_CMD_STOP;
	else
		cmd = ((u32)byte << 8) | I2C_CMD_WRITE;

	__raw_writel(cmd, pi2c->membase + I2C_CMD);
	return sprd_wait_trx_done(pi2c);
}

static int sprd_i2c_read_byte(struct sprd_i2c *pi2c, u8 *byte, int stop)
{
	int rc;
	u32 cmd;

	if (stop)
		cmd = I2C_CMD_READ | I2C_CMD_STOP | I2C_CMD_TX_ACK;
	else
		cmd = I2C_CMD_READ;

	__raw_writel(cmd, pi2c->membase + I2C_CMD);

	rc = sprd_wait_trx_done(pi2c);
	if (rc < 0)
		return rc;

	*byte = (u8)(__raw_readl(pi2c->membase + I2C_CMD) >> 8);
	return rc;
}

static int sprd_i2c_writebytes(struct sprd_i2c *pi2c, const u8 *buf, int count,
			       int is_last_msg)
{
	int i, rc = 0;

	for (i = 0; rc >= 0 && i < count; i++)
		rc = sprd_i2c_write_byte(pi2c, buf[i], i == count - 1, is_last_msg);
	return rc;
}

static int sprd_i2c_readbytes(struct sprd_i2c *pi2c, u8 *buf, int count)
{
	int i, rc = 0;

	for (i = 0; rc >= 0 && i < count; i++)
		rc = sprd_i2c_read_byte(pi2c, &buf[i], i == count - 1);
	return rc;
}

static int sprd_i2c_send_target_addr(struct sprd_i2c *pi2c, struct i2c_msg *msg)
{
	int rc;
	u32 cmd = 0, cmd2 = 0, tmp;

	if (msg->flags & I2C_M_TEN) {
		cmd = 0xf0 | (((msg->addr >> 8) & 0x03) << 1);
		cmd2 = msg->addr & 0xff;
	} else {
		cmd = (msg->addr & 0x7f) << 1;
	}

	if (msg->flags & I2C_M_RD)
		cmd |= 1;

	tmp = __raw_readl(pi2c->membase + I2C_CTL);
	__raw_writel(tmp | I2C_CTL_EN | I2C_CTL_IE, pi2c->membase + I2C_CTL);

	cmd = (cmd << 8) | I2C_CMD_START | I2C_CMD_WRITE;
	__raw_writel(cmd, pi2c->membase + I2C_CMD);

	rc = sprd_wait_trx_done(pi2c);
	if (rc < 0)
		return rc;

	if ((msg->flags & I2C_M_TEN) && !(msg->flags & I2C_M_RD)) {
		cmd2 = (cmd2 << 8) | I2C_CMD_WRITE;
		__raw_writel(cmd2, pi2c->membase + I2C_CMD);
		rc = sprd_wait_trx_done(pi2c);
	}

	return rc;
}

static int sprd_i2c_handle_msg(struct i2c_adapter *i2c_adap, struct i2c_msg *pmsg,
			       int is_last_msg)
{
	struct sprd_i2c *pi2c = i2c_adap->algo_data;
	int rc;

	rc = sprd_i2c_send_target_addr(pi2c, pmsg);
	if (rc < 0)
		return rc;

	if (pmsg->flags & I2C_M_RD)
		return sprd_i2c_readbytes(pi2c, pmsg->buf, pmsg->len);
	else
		return sprd_i2c_writebytes(pi2c, pmsg->buf, pmsg->len, is_last_msg);
}

static int sprd_i2c_master_xfer(struct i2c_adapter *i2c_adap, struct i2c_msg *msgs,
				int num)
{
	struct sprd_i2c *pi2c = i2c_adap->algo_data;
	int im, ret = 0;

	sprd_i2c_hw_clk_enable(pi2c, true);
	for (im = 0; ret >= 0 && im < num; im++)
		ret = sprd_i2c_handle_msg(i2c_adap, &msgs[im], im == num - 1);
	if (ret < 0) {
		/* Clean up bus after error so controller/bus does not hang */
		__raw_writel(I2C_CMD_STOP, pi2c->membase + I2C_CMD);
		udelay(10);
		__raw_writel(I2C_RST_RST, pi2c->membase + I2C_RST);
		udelay(10);
		__raw_writel(0, pi2c->membase + I2C_RST);
		udelay(10);
		sprd_i2c_set_clk(pi2c, pi2c->bus_freq);
	}
	sprd_i2c_hw_clk_enable(pi2c, false);

	return (ret >= 0) ? im : -EIO;
}

static u32 sprd_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm sprd_i2c_algo = {
	.master_xfer	= sprd_i2c_master_xfer,
	.functionality	= sprd_i2c_func,
};

static void sprd_i2c_set_clk(struct sprd_i2c *pi2c, unsigned int freq)
{
	unsigned int apb_clk = 26000000;
	unsigned int i2c_div;

	if (!freq)
		freq = 100000;
	i2c_div = apb_clk / (4 * freq) - 1;

	__raw_writel(i2c_div & 0xffff, pi2c->membase + I2C_CLKD0);
	__raw_writel(i2c_div >> 16, pi2c->membase + I2C_CLKD1);
}

void sprd_i2c_ctl_chg_clk(unsigned int id_nr, unsigned int freq)
{
	unsigned int tmp;
	struct sprd_i2c *pi2c;

	if (id_nr >= SPRD_I2C_CTL_ID || !sprd_i2c_ctl_id[id_nr])
		return;

	pi2c = sprd_i2c_ctl_id[id_nr];
	sprd_i2c_hw_clk_enable(pi2c, true);

	tmp = __raw_readl(pi2c->membase + I2C_CTL);
	__raw_writel(tmp & ~I2C_CTL_EN, pi2c->membase + I2C_CTL);
	sprd_i2c_set_clk(pi2c, freq);
	tmp = __raw_readl(pi2c->membase + I2C_CTL);
	__raw_writel(tmp | I2C_CTL_EN, pi2c->membase + I2C_CTL);

	sprd_i2c_hw_clk_enable(pi2c, false);
}
EXPORT_SYMBOL_GPL(sprd_i2c_ctl_chg_clk);

static void sprd_i2c_enable(struct sprd_i2c *pi2c)
{
	unsigned int tmp;

	tmp = __raw_readl(pi2c->membase + I2C_CTL);
	__raw_writel(tmp & ~(I2C_CTL_EN | I2C_CTL_IE | I2C_CTL_CMDBUF_EN),
		     pi2c->membase + I2C_CTL);

	sprd_i2c_set_clk(pi2c, pi2c->bus_freq);

	tmp = __raw_readl(pi2c->membase + I2C_CTL);
	__raw_writel(tmp | I2C_CTL_EN | I2C_CTL_IE, pi2c->membase + I2C_CTL);
	__raw_writel(I2C_CMD_INT_ACK, pi2c->membase + I2C_CMD);
}

static int sprd_i2c_probe(struct platform_device *pdev)
{
	struct sprd_i2c *pi2c;
	struct resource *res;
	struct device_node *np = pdev->dev.of_node;
	char clk_name[32];
	int ret;

	if (np) {
		int id = of_alias_get_id(np, "i2c");
		if (id >= 0)
			pdev->id = id;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	if (pdev->id < 0)
		pdev->id = (res->start - 0x70500000UL) >> 20;

	pi2c = devm_kzalloc(&pdev->dev, sizeof(*pi2c), GFP_KERNEL);
	if (!pi2c)
		return -ENOMEM;

	pi2c->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pi2c->membase))
		return PTR_ERR(pi2c->membase);

	pi2c->bus_freq = 100000;
	if (np)
		of_property_read_u32(np, "clock-frequency", &pi2c->bus_freq);

	i2c_set_adapdata(&pi2c->adap, pi2c);
	snprintf(pi2c->adap.name, sizeof(pi2c->adap.name), "sprd-i2c.%d", pdev->id);
	pi2c->adap.owner = THIS_MODULE;
	pi2c->adap.retries = 3;
	pi2c->adap.algo = &sprd_i2c_algo;
	pi2c->adap.algo_data = pi2c;
	pi2c->adap.dev.parent = &pdev->dev;
	pi2c->adap.nr = pdev->id;
	pi2c->adap.dev.of_node = np;

	snprintf(clk_name, sizeof(clk_name), "clk_i2c%d", pdev->id);
	pi2c->clk = clk_get(&pdev->dev, clk_name);
	if (IS_ERR(pi2c->clk) && np)
		pi2c->clk = of_clk_get(np, 0);

	sprd_i2c_hw_clk_enable(pi2c, true);
	sprd_i2c_enable(pi2c);
	sprd_i2c_hw_clk_enable(pi2c, false);

	ret = i2c_add_numbered_adapter(&pi2c->adap);
	if (ret < 0) {
		dev_err(&pdev->dev, "add_numbered_adapter failed: %d\n", ret);
		return ret;
	}

	if (pdev->id >= 0 && pdev->id < SPRD_I2C_CTL_ID)
		sprd_i2c_ctl_id[pdev->id] = pi2c;

	platform_set_drvdata(pdev, pi2c);
	dev_info(&pdev->dev, "registered %s at 0x%08lx (%u Hz)\n",
		 pi2c->adap.name, (unsigned long)res->start, pi2c->bus_freq);
	return 0;
}

static const struct of_device_id sprd_i2c_of_match[] = {
	{ .compatible = "sprd,i2c", },
	{ .compatible = "sprd,sp8830-i2c", },
	{ .compatible = "sprd,sc8830-i2c", },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_i2c_of_match);

static struct platform_driver sprd_i2c_driver = {
	.probe	= sprd_i2c_probe,
	.driver	= {
		.name		= "sprd-i2c",
		.of_match_table	= sprd_i2c_of_match,
	},
};

static int __init sprd_i2c_init(void)
{
	return platform_driver_register(&sprd_i2c_driver);
}
subsys_initcall(sprd_i2c_init);
