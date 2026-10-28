/*
 * Early Framebuffer Console & Samsung sec_log (/proc/last_kmsg) for SM-T560 / SM-T561
 *
 * Provides two ultra-early, zero-dependency diagnostic channels active from
 * .map_io (before SMP, timers, IRQ, or memory allocators run):
 *
 * 1. Samsung sec_log ring buffer at physical 0x86B00000 (size 0xFFE00):
 *    Matches the exact struct sec_log_buffer + sec_log_ptr + LOGM magic
 *    layout used by Samsung S-Boot and the 3.10 TWRP recovery kernel, so
 *    /proc/last_kmsg works across reboots between 4.9 and 3.10 TWRP.
 *
 * 2. Early direct framebuffer console at physical 0x9EA44000 (800x1280 BGRA32):
 *    Draws kernel printk output directly onto the S-Boot display surface
 *    starting at 0.000000s, even before simplefb/fbcon is initialized.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/font.h>
#include <asm/mach/map.h>

#include <soc/sprd/iomap.h>
#include "core.h"

/* Virtual mapping addresses above SCI_IOMAP peripherals */
#define SPRD_SECLOG_VIRT	0xF5400000UL
#define SPRD_SECLOG_PHYS	0x86B00000UL
#define SPRD_SECLOG_MAP_SIZE	SZ_1M

#define SPRD_EARLYFB_VIRT	0xF5800000UL
#define SPRD_EARLYFB_PHYS	0x9EA44000UL
#define SPRD_EARLYFB_MAP_SIZE	(SZ_4M + SZ_1M)

/* Samsung sec_log binary header (20 bytes -> 20-byte aligned in 3.10) */
struct sec_log_buffer {
	u32 sig;
	u32 start;
	u32 size;
	u32 from;
	u32 to;
};

#define SEC_LOG_MAGIC		0x4d474f4cU	/* "LOGM" */
#define SEC_RAM_CONSOLE_SIG	0x43474244U	/* "DBGC" */
#define SEC_LOG_BUF_SIZE	0x000FFE00U

static struct sec_log_buffer *sec_log_hdr;
static char *sec_log_buf;
static u32 *sec_log_ptr;
static u32 *sec_log_mag;

static char *saved_last_kmsg;
static u32 saved_last_kmsg_len;
static char early_old_log_stash[SZ_64K] __initdata;
static u32 early_old_log_len __initdata;

/* Framebuffer geometry (800x1280x32bpp) */
#define FB_WIDTH		800
#define FB_HEIGHT		1280
#define FB_STRIDE_PIXELS	800
#define FONT_W			8
#define FONT_H			16
#define FB_COLS			(FB_WIDTH / FONT_W)
#define FB_ROWS			(FB_HEIGHT / FONT_H)

static u32 __iomem *early_fb_base;
static int fb_cur_col;
static int fb_cur_row;
static bool early_fb_active = true;

static struct map_desc sprd_early_debug_io_desc[] __initdata = {
	{
		.virtual	= SPRD_SECLOG_VIRT,
		.pfn		= __phys_to_pfn(SPRD_SECLOG_PHYS),
		.length		= SPRD_SECLOG_MAP_SIZE,
		.type		= MT_MEMORY_RWX_NONCACHED,
	},
	{
		.virtual	= (SPRD_EARLYFB_VIRT),
		.pfn		= __phys_to_pfn(0x9EA00000UL),
		.length		= SPRD_EARLYFB_MAP_SIZE,
		.type		= MT_MEMORY_RWX_NONCACHED,
	},
};

static void sec_log_putc(char c)
{
	u32 pos;

	if (!sec_log_buf || !sec_log_ptr || !sec_log_hdr)
		return;

	pos = *sec_log_ptr;
	/*
	 * TWRP 3.10 sec_log_save_old() indexes sec_log_buf using
	 * `& (0xFFE00 - 1)` = `& 0xFFDFF`, which has bit 9 (0x200) cleared!
	 * Skip odd 512-byte blocks (where bit 9 is 1) and mirror the previous
	 * 512-byte block into them so TWRP /proc/last_kmsg never drops a byte.
	 */
	if (pos & 0x200) {
		u32 base512 = pos & ~0x3FFU;
		if (base512 + 0x400 <= SEC_LOG_BUF_SIZE)
			memcpy(sec_log_buf + base512 + 0x200,
			       sec_log_buf + base512, 0x200);
		pos = (pos + 0x200) & ~0x200U;
	}
	if (pos >= SEC_LOG_BUF_SIZE)
		pos = 0;

	sec_log_buf[pos] = c;
	if (pos + 0x200 < SEC_LOG_BUF_SIZE)
		sec_log_buf[pos + 0x200] = c;
	pos++;
	if (pos & 0x200)
		pos += 0x200;
	if (pos >= SEC_LOG_BUF_SIZE)
		pos = 0;

	*sec_log_ptr = pos;
	sec_log_hdr->start = pos;
	if (sec_log_hdr->size < SEC_LOG_BUF_SIZE)
		sec_log_hdr->size++;
}

static void early_fb_clear_row(int row, u32 bg)
{
	u32 __iomem *dst;
	int y, x;

	if (!early_fb_base || row < 0 || row >= FB_ROWS)
		return;

	for (y = 0; y < FONT_H; y++) {
		dst = early_fb_base + ((row * FONT_H + y) * FB_STRIDE_PIXELS);
		for (x = 0; x < FB_WIDTH; x++)
			__raw_writel(bg, &dst[x]);
	}
}

static void early_fb_draw_char(int col, int row, unsigned char ch, u32 fg, u32 bg)
{
	const struct font_desc *font;
	const u8 *glyph;
	int y, x;

	if (!early_fb_base || col < 0 || col >= FB_COLS || row < 0 || row >= FB_ROWS)
		return;

	font = find_font("VGA8x16");
	if (!font || !font->data)
		return;

	glyph = (const u8 *)font->data + ((unsigned int)ch * FONT_H);

	for (y = 0; y < FONT_H; y++) {
		u8 bits = glyph[y];
		u32 __iomem *dst = early_fb_base +
			((row * FONT_H + y) * FB_STRIDE_PIXELS) + (col * FONT_W);

		for (x = 0; x < FONT_W; x++) {
			u32 color = (bits & (0x80 >> x)) ? fg : bg;
			__raw_writel(color, &dst[x]);
		}
	}
}

static void early_fb_newline(void)
{
	fb_cur_col = 0;
	fb_cur_row++;
	if (fb_cur_row >= FB_ROWS)
		fb_cur_row = 0;

	early_fb_clear_row(fb_cur_row, 0xFF001020U);
	if (fb_cur_row + 1 < FB_ROWS)
		early_fb_clear_row(fb_cur_row + 1, 0xFF002850U);
}

static void early_fb_putc(char c)
{
	if (!early_fb_base || !early_fb_active)
		return;

	if (c == '\r')
		return;

	if (c == '\n') {
		early_fb_newline();
		return;
	}

	if (fb_cur_col >= FB_COLS)
		early_fb_newline();

	early_fb_draw_char(fb_cur_col, fb_cur_row, (unsigned char)c,
			   0xFFFFFFFFU, 0xFF001020U);
	fb_cur_col++;
}

static void sprd_early_con_write(struct console *con, const char *s, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (s[i] == '\n')
			sec_log_putc('\r');
		sec_log_putc(s[i]);
		early_fb_putc(s[i]);
	}
}

static struct console sprd_early_con = {
	.name	= "sprd_early",
	.write	= sprd_early_con_write,
	.flags	= CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index	= -1,
};

void __init sprd_early_console_map_io(void)
{
	unsigned int hdr_size;
	const char *banner = "\n=== Linux 4.9.337 SM-T560/T561 (SC7730SE) Early Console Active ===\n";
	const char *p;

	iotable_init(sprd_early_debug_io_desc, ARRAY_SIZE(sprd_early_debug_io_desc));

	/*
	 * Match 3.10 sec_log.c layout at 0x86B00000:
	 * size_struct_sec_log_buffer = (((sizeof(struct sec_log_buffer) + 4) / 4) * 4) = 24 bytes
	 */
	hdr_size = (((sizeof(struct sec_log_buffer) + 4) / 4) * 4);
	sec_log_hdr = (struct sec_log_buffer *)SPRD_SECLOG_VIRT;
	sec_log_buf = (char *)(SPRD_SECLOG_VIRT + hdr_size);
	sec_log_ptr = (u32 *)(SPRD_SECLOG_VIRT + hdr_size + SEC_LOG_BUF_SIZE);
	sec_log_mag = (u32 *)(SPRD_SECLOG_VIRT + hdr_size + SEC_LOG_BUF_SIZE + sizeof(u32));

	/* Save previous boot's tail into early_old_log_stash before writing new logs */
	if (*sec_log_mag == SEC_LOG_MAGIC && *sec_log_ptr > 0) {
		u32 ptr = *sec_log_ptr;
		u32 copy_len = min_t(u32, ptr, sizeof(early_old_log_stash));
		u32 start_off = (ptr >= copy_len) ? (ptr - copy_len) : 0;

		if (ptr <= SEC_LOG_BUF_SIZE) {
			memcpy(early_old_log_stash, sec_log_buf + start_off, copy_len);
			early_old_log_len = copy_len;
		}
	}

	*sec_log_mag = SEC_LOG_MAGIC;
	*sec_log_ptr = 0;

	sec_log_hdr->sig = SEC_RAM_CONSOLE_SIG;
	sec_log_hdr->start = 0;
	sec_log_hdr->size = 0;
	sec_log_hdr->from = 0;
	sec_log_hdr->to = 0;

	/* Framebuffer starts at physical 0x9EA44000 -> offset 0x44000 inside 0x9EA00000 map */
	early_fb_base = (u32 __iomem *)(SPRD_EARLYFB_VIRT + 0x44000UL);
	fb_cur_col = 0;
	fb_cur_row = 0;
	early_fb_clear_row(0, 0xFF001020U);
	early_fb_clear_row(1, 0xFF002850U);

	for (p = banner; *p; p++) {
		if (*p == '\n')
			sec_log_putc('\r');
		sec_log_putc(*p);
	}

	register_console(&sprd_early_con);
}

static ssize_t last_kmsg_read(struct file *file, char __user *buf,
			      size_t len, loff_t *offset)
{
	loff_t pos = *offset;
	ssize_t count;

	if (!saved_last_kmsg || pos >= saved_last_kmsg_len)
		return 0;

	count = min_t(size_t, len, saved_last_kmsg_len - pos);
	if (copy_to_user(buf, saved_last_kmsg + pos, count))
		return -EFAULT;

	*offset += count;
	return count;
}

static const struct file_operations last_kmsg_fops = {
	.owner	= THIS_MODULE,
	.read	= last_kmsg_read,
};

static int __init sprd_last_kmsg_init(void)
{
	struct proc_dir_entry *entry;

	if (early_old_log_len > 0) {
		saved_last_kmsg = kmemdup(early_old_log_stash, early_old_log_len, GFP_KERNEL);
		if (saved_last_kmsg)
			saved_last_kmsg_len = early_old_log_len;
	}

	if (!saved_last_kmsg)
		return 0;

	entry = proc_create("last_kmsg", S_IFREG | S_IRUGO, NULL, &last_kmsg_fops);
	if (entry)
		proc_set_size(entry, saved_last_kmsg_len);

	return 0;
}
late_initcall(sprd_last_kmsg_init);
