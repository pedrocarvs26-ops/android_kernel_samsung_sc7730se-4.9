/*
 * memdump - copy a physical memory range out of /dev/mem from a 32 bit
 *           recovery, where dd cannot reach it.
 *
 * Why this exists
 * ---------------
 * On the SC7730SE, DRAM starts at 0x80000000, so *every* interesting physical
 * address is above 2 GiB and does not fit in a signed 32 bit off_t. Recovery
 * shells hit that wall in two different ways:
 *
 *   - dd computes skip*bs in a 32 bit off_t, the lseek overflows or is
 *     rejected, and dd silently falls back to "read and discard" starting at
 *     physical address 0. Address 0 is below PHYS_OFFSET, so read_mem() in
 *     drivers/char/mem.c fails valid_phys_addr_range() and returns EFAULT:
 *     that is the "dd: /dev/mem: Bad address" message.
 *   - even with a correct 64 bit seek, read() on /dev/mem must pass
 *     valid_phys_addr_range() (arch/arm/mm/mmap.c), which rejects anything
 *     past the end of lowmem or inside a carved out region.
 *
 * memdump does the seek with pread() on a _FILE_OFFSET_BITS=64 build, and if
 * the kernel still refuses the read it falls back to mmap(), which only has to
 * pass valid_mmap_phys_addr_range() - that one accepts any address inside the
 * physical address space.
 *
 * It also writes a plain text version of the dump next to the binary, so no
 * `strings` binary is needed in the recovery.
 *
 * Build
 * -----
 *     sudo apt-get install -y gcc-arm-linux-gnueabihf libc6-dev-armhf-cross
 *     arm-linux-gnueabihf-gcc -static -O2 -Wall -o memdump tools/memdump.c
 *
 * libc6-dev-armhf-cross is not optional. The kernel is freestanding and builds
 * with the compiler alone, but this is ordinary userspace code: without the
 * armhf libc headers the Debian cross compiler ends its include search in the
 * host /usr/include, picks up the x86_64 <errno.h> and stops at
 * "sys/cdefs.h: No such file or directory". The package also carries the
 * libc.a that -static needs.
 *
 * Use (TWRP terminal, right after a warm reboot)
 * ----------------------------------------------
 *     ./memdump                    # 384 KiB at 0x86b80000 -> /sdcard/ramoops.bin
 *     ./memdump 0x86b80000 0x60000 /sdcard/ramoops.bin
 *     ./memdump --probe            # 32 bytes at each known log address
 *     ./memdump --seclog           # the whole bootloader sec_log window
 *
 * There is no blind scan any more
 * -------------------------------
 * The first version had a --scan mode that walked 0x80000000..0xa0000000 a
 * megabyte at a time. On the device it reset the tablet on the spot, before
 * printing a single hit. That is expected in hindsight: parts of that range
 * are TrustZone carve-outs and modem windows, a non-secure read of those
 * aborts on the bus, and the SoC turns the abort into an immediate reboot.
 * --probe replaces it and only touches addresses that can legitimately hold a
 * log header, 32 bytes each.
 */

#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE 1
#define _GNU_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Where the RAM console lives, see docs/DEBUG-TWRP.md. The bootloader passes
 * "sec_log=0xffe00@0x86b00000" to the stock kernel, so it keeps that megabyte
 * out of the usable memory map on every boot, recovery included. Our ramoops
 * zone sits in the upper half of that window, out of reach of the recovery
 * kernel's own sec_log writes, which start at the base.
 */
#define DEFAULT_ADDR 0x86b80000ULL
#define DEFAULT_SIZE 0x60000ULL
#define DEFAULT_OUT "/sdcard/ramoops.bin"

/* The window the bootloader reserves, and the vendor layout inside it. */
#define SECLOG_BASE 0x86b00000ULL
#define SECLOG_SIZE 0xffe00ULL
/* struct sec_log_buffer {sig,start,size,from,to}, rounded up to 4 bytes */
#define SECLOG_HDR 24

/* PERSISTENT_RAM_SIG from fs/pstore/ram_core.c, 'DBGC' */
#define RAMOOPS_SIG 0x43474244U
/* Samsung sec_log / ram_console magic seen in the 3.10 vendor tree, 'LOGM' */
#define SEC_LOG_MAGIC 0x4d474f4cU

static int mem_fd = -1;
static long page_size = 4096;
static int used_mmap = 0;

/* Read len bytes of physical memory. Tries pread() first, then mmap(). */
static int read_phys(uint64_t phys, unsigned char *buf, size_t len)
{
	size_t done = 0;

	if (!used_mmap) {
		while (done < len) {
			ssize_t n = pread(mem_fd, buf + done, len - done,
					  (off_t)(phys + done));
			if (n > 0) {
				done += (size_t)n;
				continue;
			}
			if (n == 0)
				break;
			if (done == 0)
				break;   /* nothing read at all: try mmap */
			return -1;
		}
		if (done == len)
			return 0;
	}

	{
		uint64_t base = phys & ~((uint64_t)page_size - 1);
		size_t delta = (size_t)(phys - base);
		size_t maplen = len + delta;
		void *p;

		maplen = (maplen + (size_t)page_size - 1) &
			 ~((size_t)page_size - 1);
		p = mmap(NULL, maplen, PROT_READ, MAP_SHARED, mem_fd,
			 (off_t)base);
		if (p == MAP_FAILED)
			return -1;
		memcpy(buf, (unsigned char *)p + delta, len);
		munmap(p, maplen);
		used_mmap = 1;
		return 0;
	}
}

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* Write the printable runs of the dump to <out>.txt and echo the tail. */
static void write_text(const char *out, const unsigned char *buf, size_t len)
{
	char path[512];
	FILE *f;
	size_t i, run = 0, kept = 0;
	unsigned char *tmp = malloc(len + 1);

	if (!tmp)
		return;
	snprintf(path, sizeof(path), "%s.txt", out);
	f = fopen(path, "wb");
	if (!f) {
		free(tmp);
		return;
	}

	for (i = 0; i <= len; i++) {
		int c = (i < len) ? buf[i] : -1;
		int printable = (c == '\n' || c == '\t' ||
				 (c >= 0x20 && c <= 0x7e));
		if (printable) {
			tmp[run++] = (unsigned char)c;
			continue;
		}
		if (run >= 4) {
			fwrite(tmp, 1, run, f);
			if (tmp[run - 1] != '\n')
				fputc('\n', f);
			kept += run;
		}
		run = 0;
	}
	fclose(f);
	free(tmp);
	printf("text     : %s (%lu printable bytes)\n", path,
	       (unsigned long)kept);
}

/* Read the bootloader's "sec_log=SIZE@BASE", so we probe the real window. */
static int parse_cmdline_seclog(uint64_t *base, uint64_t *size)
{
	char line[4096];
	char *p;
	FILE *f = fopen("/proc/cmdline", "r");

	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	p = strstr(line, "sec_log=");
	if (!p)
		return -1;
	p += 8;
	*size = strtoull(p, &p, 0);
	if (*p == 'K' || *p == 'k') {
		*size *= 1024;
		p++;
	} else if (*p == 'M' || *p == 'm') {
		*size *= 1024 * 1024;
		p++;
	}
	if (*p != '@')
		return -1;
	*base = strtoull(p + 1, NULL, 0);
	return (*base && *size) ? 0 : -1;
}

static const char *sig_name(uint32_t sig)
{
	if (sig == RAMOOPS_SIG)
		return "'DBGC' ramoops/sec_log header";
	if (sig == SEC_LOG_MAGIC)
		return "'LOGM' vendor sec_log magic";
	if (!sig)
		return "zeros";
	return "no log header";
}

/*
 * Read 32 bytes at each address that can legitimately hold a log header.
 * Never walk memory blindly, see the note at the top of this file.
 */
static int do_probe(void)
{
	struct {
		uint64_t addr;
		const char *what;
	} cand[5];
	uint64_t base = 0, size = 0;
	unsigned char b[32];
	int i, n = 0, hits = 0;

	cand[n].addr = DEFAULT_ADDR;
	cand[n++].what = "our ramoops zone";
	cand[n].addr = SECLOG_BASE;
	cand[n++].what = "sec_log window, vendor header";
	cand[n].addr = SECLOG_BASE + SECLOG_HDR;
	cand[n++].what = "sec_log window, first text byte";
	cand[n].addr = 0x89b00000ULL;
	cand[n++].what = "old ramoops address (modem memory)";

	if (parse_cmdline_seclog(&base, &size) == 0) {
		printf("cmdline  : sec_log=0x%llx@0x%llx\n",
		       (unsigned long long)size, (unsigned long long)base);
		if (base != SECLOG_BASE) {
			cand[n].addr = base;
			cand[n++].what = "sec_log base from /proc/cmdline";
		}
	} else {
		printf("cmdline  : no sec_log= parameter\n");
	}

	printf("address     word        meaning                        region\n");
	for (i = 0; i < n; i++) {
		memset(b, 0, sizeof(b));
		if (read_phys(cand[i].addr, b, sizeof(b)) < 0) {
			printf("0x%08llx  unreadable  %-30s %s\n",
			       (unsigned long long)cand[i].addr,
			       strerror(errno), cand[i].what);
			continue;
		}
		printf("0x%08llx  0x%08x  %-30s %s\n",
		       (unsigned long long)cand[i].addr, le32(b),
		       sig_name(le32(b)), cand[i].what);
		if (le32(b) == RAMOOPS_SIG || le32(b) == SEC_LOG_MAGIC)
			hits++;
	}
	printf("%d header(s) found\n", hits);
	return hits ? 0 : 2;
}

/*
 * Dump the whole window the bootloader reserves for the stock RAM console and
 * decode the vendor layout, from arch/arm/mach-sc/sec_log.c in the 3.10 tree:
 *
 *   base + 0                struct sec_log_buffer {sig,start,size,from,to}
 *   base + 24               text, sec_logbuf_size bytes
 *   base + 24 + size        sec_log_ptr, the write index
 *   base + 24 + size + 4    sec_log_mag, 'LOGM' once initialised
 *
 * Running this from a normal TWRP session is the cheap way to prove the window
 * is real: the recovery kernel's own boot log should be sitting in it.
 */
static int do_seclog(const char *out)
{
	uint64_t base = SECLOG_BASE, size = SECLOG_SIZE, total;
	unsigned char *buf;
	uint32_t mag;
	FILE *f;

	if (parse_cmdline_seclog(&base, &size) == 0)
		printf("cmdline  : sec_log=0x%llx@0x%llx\n",
		       (unsigned long long)size, (unsigned long long)base);
	else
		printf("cmdline  : no sec_log=, using 0x%llx@0x%llx\n",
		       (unsigned long long)size, (unsigned long long)base);

	total = SECLOG_HDR + size + 8;
	buf = malloc((size_t)total);
	if (!buf) {
		fprintf(stderr, "memdump: cannot allocate %llu bytes\n",
			(unsigned long long)total);
		return 1;
	}
	memset(buf, 0, (size_t)total);
	if (read_phys(base, buf, (size_t)total) < 0) {
		fprintf(stderr, "memdump: reading 0x%08llx: %s\n",
			(unsigned long long)base, strerror(errno));
		free(buf);
		return 1;
	}

	f = fopen(out, "wb");
	if (!f) {
		fprintf(stderr, "memdump: %s: %s\n", out, strerror(errno));
		free(buf);
		return 1;
	}
	fwrite(buf, 1, (size_t)total, f);
	fclose(f);

	mag = le32(buf + (size_t)(SECLOG_HDR + size) + 4);
	printf("read     : 0x%08llx + 0x%llx via %s\n",
	       (unsigned long long)base, (unsigned long long)total,
	       used_mmap ? "mmap" : "pread");
	printf("binary   : %s\n", out);
	printf("header   : sig 0x%08x start %u size %u from %u to %u\n",
	       le32(buf), le32(buf + 4), le32(buf + 8), le32(buf + 12),
	       le32(buf + 16));
	printf("write ptr: %u\n", le32(buf + (size_t)(SECLOG_HDR + size)));
	printf("magic    : 0x%08x %s\n", mag,
	       mag == SEC_LOG_MAGIC ?
	       "('LOGM': the recovery kernel owns this buffer)" :
	       "(not initialised by this kernel)");
	write_text(out, buf, (size_t)total);
	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t addr = DEFAULT_ADDR;
	uint64_t size = DEFAULT_SIZE;
	const char *out = DEFAULT_OUT;
	unsigned char *buf;
	FILE *f;
	int probe = 0, seclog = 0;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = 4096;

	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		printf("usage: memdump [<phys_addr> <size> [outfile]]\n"
		       "       memdump --probe            known log addresses only\n"
		       "       memdump --seclog [outfile] the bootloader sec_log window\n"
		       "default: 0x%08llx 0x%llx %s\n",
		       (unsigned long long)DEFAULT_ADDR,
		       (unsigned long long)DEFAULT_SIZE, DEFAULT_OUT);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--scan")) {
		fprintf(stderr,
			"memdump: --scan is gone. Walking 0x80000000..0xa0000000\n"
			"         blindly crosses TrustZone and modem carve-outs, and\n"
			"         the bus abort that follows resets the tablet before\n"
			"         anything can be printed. Use --probe or --seclog.\n");
		return 1;
	}
	if (argc > 1 && !strcmp(argv[1], "--probe"))
		probe = 1;
	else if (argc > 1 && !strcmp(argv[1], "--seclog"))
		seclog = 1;
	else if (argc >= 3) {
		addr = strtoull(argv[1], NULL, 0);
		size = strtoull(argv[2], NULL, 0);
		if (argc >= 4)
			out = argv[3];
	}

	mem_fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (mem_fd < 0) {
		fprintf(stderr, "memdump: /dev/mem: %s\n", strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "try: mknod /dev/mem c 1 1\n");
		return 1;
	}

	if (probe)
		return do_probe();
	if (seclog)
		return do_seclog(argc >= 3 ? argv[2] : "/sdcard/seclog.bin");

	buf = malloc((size_t)size);
	if (!buf) {
		fprintf(stderr, "memdump: cannot allocate %llu bytes\n",
			(unsigned long long)size);
		return 1;
	}
	memset(buf, 0, (size_t)size);

	if (read_phys(addr, buf, (size_t)size) < 0) {
		fprintf(stderr, "memdump: reading 0x%08llx: %s\n",
			(unsigned long long)addr, strerror(errno));
		fprintf(stderr, "neither read() nor mmap() worked on this kernel\n");
		return 1;
	}

	f = fopen(out, "wb");
	if (!f) {
		fprintf(stderr, "memdump: %s: %s\n", out, strerror(errno));
		return 1;
	}
	fwrite(buf, 1, (size_t)size, f);
	fclose(f);

	printf("read     : 0x%08llx + 0x%llx via %s\n",
	       (unsigned long long)addr, (unsigned long long)size,
	       used_mmap ? "mmap" : "pread");
	printf("binary   : %s\n", out);

	{
		uint32_t sig = le32(buf);

		if (sig == RAMOOPS_SIG)
			printf("signature: 0x%08x 'DBGC' - ramoops header present, "
			       "start=%u size=%u\n", sig, le32(buf + 4),
			       le32(buf + 8));
		else if (sig == SEC_LOG_MAGIC)
			printf("signature: 0x%08x 'LOGM' - vendor sec_log buffer\n",
			       sig);
		else
			printf("signature: 0x%08x - no log header here; try "
			       "./memdump --probe\n", sig);
	}

	write_text(out, buf, (size_t)size);
	free(buf);
	return 0;
}
