/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) D-Die & A-Die GPIO and EIC Driver
 *
 * Adapted from drivers/gpio/gpio-eic.c in the 3.10 kernel for Linux 4.9.337.
 * Supports:
 *   - sprd,d-eic-gpio  (0x40210000, gpiobase 304, ngpios 16)
 *   - sprd,d-gpio-gpio (0x40280000, gpiobase 0,   ngpios 240)
 *   - sprd,a-eic-gpio  (0x40038100, gpiobase 320, ngpios 16)
 *   - sprd,a-gpio-gpio (0x40038480, gpiobase 272, ngpios 32)
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/gpio/driver.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>

#include <soc/sprd/iomap.h>

#define GPIO_GROUP_NR		16
#define GPIO_GROUP_MASK		0xFFFF
#define GPIO_GROUP_OFFSET	0x80
#define ANA_GPIO_GROUP_OFFSET	0x40

#define REG_GPIO_DATA		0x0000
#define REG_GPIO_DMSK		0x0004
#define REG_GPIO_DIR		0x0008
#define REG_GPIO_IS		0x000C
#define REG_GPIO_IBE		0x0010
#define REG_GPIO_IEV		0x0014
#define REG_GPIO_IE		0x0018
#define REG_GPIO_RIS		0x001C
#define REG_GPIO_MIS		0x0020
#define REG_GPIO_IC		0x0024
#define REG_GPIO_INEN		0x0028

#define REG_EIC_IEV		REG_GPIO_IEV
#define REG_EIC_IE		REG_GPIO_IE
#define REG_EIC_MIS		REG_GPIO_MIS
#define REG_EIC_IC		REG_GPIO_IC
#define REG_EIC_TRIG		0x0028

struct sci_gpio_chip {
	struct gpio_chip chip;
	unsigned long base_addr;
	u32 group_offset;
	struct irq_domain *irq_domain;
	int is_adi_gpio;

	u32 (*read_reg)(unsigned long addr);
	void (*write_reg)(u32 value, unsigned long addr);
	void (*set_bits)(u32 bits, unsigned long addr);
	void (*clr_bits)(u32 bits, unsigned long addr);
};

#define to_sci_gpio(c) container_of(c, struct sci_gpio_chip, chip)

static u32 d_read_reg(unsigned long addr)
{
	return __raw_readl((void __iomem *)addr);
}

static void d_write_reg(u32 value, unsigned long addr)
{
	__raw_writel(value, (void __iomem *)addr);
}

static void d_set_bits(u32 bits, unsigned long addr)
{
	__raw_writel(__raw_readl((void __iomem *)addr) | bits, (void __iomem *)addr);
}

static void d_clr_bits(u32 bits, unsigned long addr)
{
	__raw_writel(__raw_readl((void __iomem *)addr) & ~bits, (void __iomem *)addr);
}

static u32 a_read_reg(unsigned long addr)
{
	return (u32)sci_adi_read(addr);
}

static void a_write_reg(u32 value, unsigned long addr)
{
	sci_adi_raw_write(addr, (u16)value);
}

static void a_set_bits(u32 bits, unsigned long addr)
{
	sci_adi_set(addr, (u16)bits);
}

static void a_clr_bits(u32 bits, unsigned long addr)
{
	sci_adi_clr(addr, (u16)bits);
}

static int sci_gpio_read(struct gpio_chip *chip, u32 offset, u32 reg)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(chip);
	int group = offset / GPIO_GROUP_NR;
	int bitof = offset & (GPIO_GROUP_NR - 1);
	unsigned long addr = sci_gpio->base_addr + sci_gpio->group_offset * group + reg;
	u32 value = sci_gpio->read_reg(addr) & GPIO_GROUP_MASK;

	return (value >> bitof) & 0x1;
}

static void sci_gpio_write(struct gpio_chip *chip, u32 offset, u32 reg, int value)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(chip);
	int group = offset / GPIO_GROUP_NR;
	int bitof = offset & (GPIO_GROUP_NR - 1);
	unsigned long addr = sci_gpio->base_addr + sci_gpio->group_offset * group + reg;

	if (value)
		sci_gpio->set_bits(1U << bitof, addr);
	else
		sci_gpio->clr_bits(1U << bitof, addr);
}

static int sci_gpio_request(struct gpio_chip *chip, unsigned int offset)
{
	sci_gpio_write(chip, offset, REG_GPIO_DMSK, 1);
	return 0;
}

static void sci_gpio_free(struct gpio_chip *chip, unsigned int offset)
{
	sci_gpio_write(chip, offset, REG_GPIO_DMSK, 0);
}

static int sci_gpio_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	sci_gpio_write(chip, offset, REG_GPIO_DIR, 0);
	sci_gpio_write(chip, offset, REG_GPIO_INEN, 1);
	return 0;
}

static int sci_eic_direction_input(struct gpio_chip *chip, unsigned int offset)
{
	return 0;
}

static int sci_gpio_direction_output(struct gpio_chip *chip, unsigned int offset,
				     int value)
{
	sci_gpio_write(chip, offset, REG_GPIO_DIR, 1);
	sci_gpio_write(chip, offset, REG_GPIO_INEN, 0);
	sci_gpio_write(chip, offset, REG_GPIO_DATA, value);
	return 0;
}

static int sci_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	return sci_gpio_read(chip, offset, REG_GPIO_DATA);
}

static void sci_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	sci_gpio_write(chip, offset, REG_GPIO_DATA, value);
}

static int sci_eic_set_debounce(struct gpio_chip *chip, unsigned int offset,
				unsigned int debounce)
{
	return 0;
}

static int sci_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(chip);

	if (!sci_gpio->irq_domain)
		return -ENXIO;
	return irq_find_mapping(sci_gpio->irq_domain, offset);
}

static int sci_irq_to_gpio(struct gpio_chip *chip, unsigned int irq)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(chip);
	int base_irq = irq_find_mapping(sci_gpio->irq_domain, 0);

	return irq - base_irq;
}

static struct sci_gpio_chip d_sci_gpio = {
	.chip = {
		.label			= "sprd-d-gpio",
		.request		= sci_gpio_request,
		.free			= sci_gpio_free,
		.direction_input	= sci_gpio_direction_input,
		.get			= sci_gpio_get,
		.direction_output	= sci_gpio_direction_output,
		.set			= sci_gpio_set,
		.to_irq			= sci_gpio_to_irq,
	},
	.group_offset	= GPIO_GROUP_OFFSET,
	.read_reg	= d_read_reg,
	.write_reg	= d_write_reg,
	.set_bits	= d_set_bits,
	.clr_bits	= d_clr_bits,
	.is_adi_gpio	= 0,
};

static struct sci_gpio_chip a_sci_gpio = {
	.chip = {
		.label			= "sprd-a-gpio",
		.request		= sci_gpio_request,
		.free			= sci_gpio_free,
		.direction_input	= sci_gpio_direction_input,
		.get			= sci_gpio_get,
		.direction_output	= sci_gpio_direction_output,
		.set			= sci_gpio_set,
		.to_irq			= sci_gpio_to_irq,
	},
	.group_offset	= ANA_GPIO_GROUP_OFFSET,
	.read_reg	= a_read_reg,
	.write_reg	= a_write_reg,
	.set_bits	= a_set_bits,
	.clr_bits	= a_clr_bits,
	.is_adi_gpio	= 1,
};

static struct sci_gpio_chip d_sci_eic = {
	.chip = {
		.label			= "sprd-d-eic",
		.request		= sci_gpio_request,
		.free			= sci_gpio_free,
		.direction_input	= sci_eic_direction_input,
		.get			= sci_gpio_get,
		.set_debounce		= sci_eic_set_debounce,
		.to_irq			= sci_gpio_to_irq,
	},
	.group_offset	= GPIO_GROUP_OFFSET,
	.read_reg	= d_read_reg,
	.write_reg	= d_write_reg,
	.set_bits	= d_set_bits,
	.clr_bits	= d_clr_bits,
	.is_adi_gpio	= 0,
};

static struct sci_gpio_chip a_sci_eic = {
	.chip = {
		.label			= "sprd-a-eic",
		.request		= sci_gpio_request,
		.free			= sci_gpio_free,
		.direction_input	= sci_eic_direction_input,
		.get			= sci_gpio_get,
		.set_debounce		= sci_eic_set_debounce,
		.to_irq			= sci_gpio_to_irq,
	},
	.group_offset	= ANA_GPIO_GROUP_OFFSET,
	.read_reg	= a_read_reg,
	.write_reg	= a_write_reg,
	.set_bits	= a_set_bits,
	.clr_bits	= a_clr_bits,
	.is_adi_gpio	= 1,
};

static void sci_gpio_irq_ack(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	sci_gpio_write(chip, offset, REG_GPIO_IC, 1);
}

static void sci_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	sci_gpio_write(chip, offset, REG_GPIO_IE, 0);
}

static void sci_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	sci_gpio_write(chip, offset, REG_GPIO_IE, 1);
}

static void sci_eic_irq_ack(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	sci_gpio_write(chip, offset, REG_EIC_IC, 1);
}

static void sci_eic_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	sci_gpio_write(chip, offset, REG_EIC_IE, 0);
}

static void sci_eic_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	sci_gpio_write(chip, offset, REG_EIC_IE, 1);
	sci_gpio_write(chip, offset, REG_EIC_TRIG, 1);
}

static int sci_gpio_irq_set_type(struct irq_data *data, unsigned int flow_type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	switch (flow_type) {
	case IRQ_TYPE_EDGE_RISING:
		sci_gpio_write(chip, offset, REG_GPIO_IS, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IBE, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IEV, 1);
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		sci_gpio_write(chip, offset, REG_GPIO_IS, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IBE, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IEV, 0);
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_EDGE_BOTH:
		sci_gpio_write(chip, offset, REG_GPIO_IS, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IBE, 1);
		irq_set_handler_locked(data, handle_edge_irq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		sci_gpio_write(chip, offset, REG_GPIO_IS, 1);
		sci_gpio_write(chip, offset, REG_GPIO_IBE, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IEV, 1);
		irq_set_handler_locked(data, handle_level_irq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		sci_gpio_write(chip, offset, REG_GPIO_IS, 1);
		sci_gpio_write(chip, offset, REG_GPIO_IBE, 0);
		sci_gpio_write(chip, offset, REG_GPIO_IEV, 0);
		irq_set_handler_locked(data, handle_level_irq);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sci_eic_irq_set_type(struct irq_data *data, unsigned int flow_type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	int offset = sci_irq_to_gpio(chip, data->irq);

	switch (flow_type) {
	case IRQ_TYPE_LEVEL_HIGH:
	case IRQ_TYPE_EDGE_RISING:
		sci_gpio_write(chip, offset, REG_EIC_IEV, 1);
		irq_set_handler_locked(data, handle_level_irq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
	case IRQ_TYPE_EDGE_FALLING:
	case IRQ_TYPE_EDGE_BOTH:
		sci_gpio_write(chip, offset, REG_EIC_IEV, 0);
		irq_set_handler_locked(data, handle_level_irq);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sci_gpio_irq_set_wake(struct irq_data *data, unsigned int on)
{
	return 0;
}

static struct irq_chip d_gpio_irq_chip = {
	.name		= "irq-d-gpio",
	.irq_disable	= sci_gpio_irq_mask,
	.irq_ack	= sci_gpio_irq_ack,
	.irq_mask	= sci_gpio_irq_mask,
	.irq_unmask	= sci_gpio_irq_unmask,
	.irq_set_type	= sci_gpio_irq_set_type,
	.irq_set_wake	= sci_gpio_irq_set_wake,
};

static struct irq_chip a_gpio_irq_chip = {
	.name		= "irq-a-gpio",
	.irq_disable	= sci_gpio_irq_mask,
	.irq_ack	= sci_gpio_irq_ack,
	.irq_mask	= sci_gpio_irq_mask,
	.irq_unmask	= sci_gpio_irq_unmask,
	.irq_set_type	= sci_gpio_irq_set_type,
	.irq_set_wake	= sci_gpio_irq_set_wake,
};

static struct irq_chip d_eic_irq_chip = {
	.name		= "irq-d-eic",
	.irq_disable	= sci_eic_irq_mask,
	.irq_ack	= sci_eic_irq_ack,
	.irq_mask	= sci_eic_irq_mask,
	.irq_unmask	= sci_eic_irq_unmask,
	.irq_set_type	= sci_eic_irq_set_type,
	.irq_set_wake	= sci_gpio_irq_set_wake,
};

static struct irq_chip a_eic_irq_chip = {
	.name		= "irq-a-eic",
	.irq_disable	= sci_eic_irq_mask,
	.irq_ack	= sci_eic_irq_ack,
	.irq_mask	= sci_eic_irq_mask,
	.irq_unmask	= sci_eic_irq_unmask,
	.irq_set_type	= sci_eic_irq_set_type,
	.irq_set_wake	= sci_gpio_irq_set_wake,
};

static void eic_Ack_and_handle(struct gpio_chip *chip)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(chip);
	int group, n;
	unsigned long addr;
	u32 value;

	for (group = 0; group * GPIO_GROUP_NR < chip->ngpio; group++) {
		addr = sci_gpio->base_addr + sci_gpio->group_offset * group + REG_EIC_MIS;
		value = sci_gpio->read_reg(addr) & GPIO_GROUP_MASK;

		while (value) {
			n = __ffs(value);
			value &= ~(1U << n);
			sci_gpio_write(chip, group * GPIO_GROUP_NR + n, REG_EIC_IC, 1);
			generic_handle_irq(chip->to_irq(chip, group * GPIO_GROUP_NR + n));
		}
	}
}

static void gpio_eic_handler(struct gpio_chip *chip)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(chip);
	int group, n;
	unsigned long addr;
	u32 value;

	for (group = 0; group * GPIO_GROUP_NR < chip->ngpio; group++) {
		addr = sci_gpio->base_addr + sci_gpio->group_offset * group + REG_GPIO_MIS;
		value = sci_gpio->read_reg(addr) & GPIO_GROUP_MASK;

		while (value) {
			n = __ffs(value);
			value &= ~(1U << n);
			generic_handle_irq(chip->to_irq(chip, group * GPIO_GROUP_NR + n));
		}
	}
}

static irqreturn_t gpio_muxed_handler(int irq, void *dev_id)
{
	struct gpio_chip *chip = dev_id;

	gpio_eic_handler(chip);
	return IRQ_HANDLED;
}

static irqreturn_t eic_muxed_handler(int irq, void *dev_id)
{
	struct gpio_chip *chip = dev_id;

	eic_Ack_and_handle(chip);
	return IRQ_HANDLED;
}

static irqreturn_t ana_eic_muxed_handler(int irq, void *dev_id)
{
	struct gpio_chip *chip = dev_id;
	u32 status = sci_adi_read(ANA_REG_INT_MASK_STATUS);

	if (status & BIT(5))
		eic_Ack_and_handle(chip);
	return IRQ_HANDLED;
}

static struct irqaction __d_gpio_irq = {
	.name		= "gpio",
	.flags		= IRQF_NO_SUSPEND,
	.handler	= gpio_muxed_handler,
	.dev_id		= &d_sci_gpio.chip,
};

static struct irqaction __d_eic_irq = {
	.name		= "eic",
	.flags		= IRQF_NO_SUSPEND,
	.handler	= eic_muxed_handler,
	.dev_id		= &d_sci_eic.chip,
};

static struct irqaction __a_eic_irq = {
	.name		= "ana_eic",
	.flags		= IRQF_NO_SUSPEND | IRQF_SHARED,
	.handler	= ana_eic_muxed_handler,
	.dev_id		= &a_sci_eic.chip,
};

struct sprd_gpio_match_data {
	struct sci_gpio_chip *sci_gpio_chip;
	struct irq_chip *irq_chip;
	struct irqaction *irqaction;
};

static struct sprd_gpio_match_data d_eic_match = {
	.sci_gpio_chip	= &d_sci_eic,
	.irq_chip	= &d_eic_irq_chip,
	.irqaction	= &__d_eic_irq,
};

static struct sprd_gpio_match_data d_gpio_match = {
	.sci_gpio_chip	= &d_sci_gpio,
	.irq_chip	= &d_gpio_irq_chip,
	.irqaction	= &__d_gpio_irq,
};

static struct sprd_gpio_match_data a_eic_match = {
	.sci_gpio_chip	= &a_sci_eic,
	.irq_chip	= &a_eic_irq_chip,
	.irqaction	= &__a_eic_irq,
};

static struct sprd_gpio_match_data a_gpio_match = {
	.sci_gpio_chip	= &a_sci_gpio,
	.irq_chip	= &a_gpio_irq_chip,
	.irqaction	= NULL,
};

static const struct of_device_id eic_gpio_match_table[] = {
	{ .compatible = "sprd,d-eic-gpio",  .data = &d_eic_match },
	{ .compatible = "sprd,d-gpio-gpio", .data = &d_gpio_match },
	{ .compatible = "sprd,sc8830-gpio", .data = &d_gpio_match },
	{ .compatible = "sprd,a-eic-gpio",  .data = &a_eic_match },
	{ .compatible = "sprd,a-gpio-gpio", .data = &a_gpio_match },
	{ }
};

static unsigned long sprd_gpio_phys_to_virt(unsigned long paddr, int is_adi)
{
	if (paddr >= SCI_IOMAP_BASE)
		return paddr;
	if (is_adi) {
		if (paddr >= SPRD_ADISLAVE_PHYS && paddr < SPRD_ADISLAVE_PHYS + SZ_32K)
			return (paddr - SPRD_ADISLAVE_PHYS) + SPRD_ADISLAVE_BASE;
		return paddr;
	}
	if (paddr >= SPRD_GPIO_PHYS && paddr < SPRD_GPIO_PHYS + SPRD_GPIO_SIZE)
		return (paddr - SPRD_GPIO_PHYS) + SPRD_GPIO_BASE;
	if (paddr >= SPRD_EIC_PHYS && paddr < SPRD_EIC_PHYS + SPRD_EIC_SIZE)
		return (paddr - SPRD_EIC_PHYS) + SPRD_EIC_BASE;

	return (unsigned long)ioremap(paddr, SZ_4K);
}

static void gpio_irq_init(int irq, struct gpio_chip *gpiochip,
			  struct irq_chip *irqchip)
{
	struct sci_gpio_chip *sci_gpio = to_sci_gpio(gpiochip);
	struct irq_domain *irq_domain = sci_gpio->irq_domain;
	int i, n;

	if (sci_gpio->is_adi_gpio && irq > 0) {
		/* Enable ANA EIC interrupt bit (bit 5) in ANA_REG_INT_EN */
		sci_adi_set(ANA_REG_INT_EN, BIT(5));
	}

	for (i = 0; i < gpiochip->ngpio; i++) {
		n = irq_create_mapping(irq_domain, i);
		irq_set_chip_and_handler(n, irqchip, handle_level_irq);
		irq_set_chip_data(n, gpiochip);
		irq_clear_status_flags(n, IRQ_NOREQUEST);
	}
}

static int eic_gpio_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	const struct sprd_gpio_match_data *match_data;
	struct device_node *np = pdev->dev.of_node;
	struct sci_gpio_chip *sgc;
	struct irq_chip *ic;
	struct irqaction *ia;
	static bool init_done;
	struct resource *res;
	struct irq_domain *irq_domain;
	u32 ngpios = 240, gpiobase = 0;
	int irq, ret;

	match = of_match_device(eic_gpio_match_table, &pdev->dev);
	if (!match || !match->data)
		return -ENODEV;

	if (!init_done) {
		sci_glb_set(REG_AON_APB_APB_EB0, BIT_GPIO_EB | BIT_EIC_EB);
		sci_glb_set(REG_AON_APB_APB_RTC_EB, BIT_EIC_RTC_EB | BIT_EIC_RTCDV5_EB);
		sci_adi_set(ANA_REG_GLB_ARM_MODULE_EN, BIT_ANA_EIC_EN | BIT_ANA_GPIO_EN);
		sci_adi_set(ANA_REG_GLB_RTC_CLK_EN, BIT_RTC_EIC_AG_EN);
		init_done = true;
	}

	match_data = match->data;
	sgc = match_data->sci_gpio_chip;
	ic = match_data->irq_chip;
	ia = match_data->irqaction;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	sgc->base_addr = sprd_gpio_phys_to_virt(res->start, sgc->is_adi_gpio);

	irq = platform_get_irq(pdev, 0);

	if (of_property_read_u32(np, "ngpios", &ngpios))
		ngpios = 240;
	sgc->chip.ngpio = (u16)ngpios;

	if (!of_property_read_u32(np, "gpiobase", &gpiobase))
		sgc->chip.base = (int)gpiobase;
	else
		sgc->chip.base = 0;

	sgc->chip.of_node = np;
	sgc->chip.parent = &pdev->dev;

	ret = gpiochip_add(&sgc->chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register gpiochip %s: %d\n",
			sgc->chip.label, ret);
		return ret;
	}

	irq_domain = irq_domain_add_linear(np, sgc->chip.ngpio,
					   &irq_domain_simple_ops, NULL);
	if (!irq_domain) {
		gpiochip_remove(&sgc->chip);
		return -ENOMEM;
	}
	sgc->irq_domain = irq_domain;

	if (irq > 0) {
		gpio_irq_init(irq, &sgc->chip, ic);
		if (ia)
			setup_irq(irq, ia);
	}

	dev_info(&pdev->dev, "registered %s (vbase=0x%lx, base=%d, ngpio=%d, irq=%d)\n",
		 sgc->chip.label, sgc->base_addr, sgc->chip.base, sgc->chip.ngpio, irq);
	return 0;
}

static struct platform_driver eic_gpio_driver = {
	.driver = {
		.name		= "eic-gpio",
		.of_match_table	= eic_gpio_match_table,
	},
	.probe = eic_gpio_probe,
};

static int __init eic_gpio_init(void)
{
	return platform_driver_register(&eic_gpio_driver);
}
postcore_initcall(eic_gpio_init);
