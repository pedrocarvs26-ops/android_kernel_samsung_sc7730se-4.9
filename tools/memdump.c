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
 *     ./memdump                    # 1 MiB at 0x89b00000 -> /sdcard/ramoops.bin
 *     ./memdump 0x89b00000 0x100000 /sdcard/ramoops.bin
 *     ./memdump --scan             # hunt for the ramoops signature in DRAM
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

#define DEFAULT_ADDR 0x89b00000ULL
#define DEFAULT_SIZE 0x100000ULL
#define DEFAULT_OUT "/sdcard/ramoops.bin"

/* PERSISTENT_RAM_SIG from fs/pstore/ram_core.c, 'DBGC' */
#define RAMOOPS_SIG 0x43474244U
/* Samsung sec_log / ram_console magic seen in the 3.10 vendor tree, 'LOGM' */
#define SEC_LOG_MAGIC 0x4d474f4cU

#define SCAN_START 0x80000000ULL
#define SCAN_END 0xa0000000ULL
#define SCAN_WINDOW (1024 * 1024)

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

static int do_scan(void)
{
	unsigned char *buf = malloc(SCAN_WINDOW);
	uint64_t addr;
	int hits = 0;

	if (!buf) {
		fprintf(stderr, "memdump: out of memory\n");
		return 1;
	}
	printf("scanning 0x%llx..0x%llx for ramoops ('DBGC') and sec_log ('LOGM')\n",
	       (unsigned long long)SCAN_START, (unsigned long long)SCAN_END);
	for (addr = SCAN_START; addr < SCAN_END; addr += SCAN_WINDOW) {
		size_t off;

		if (read_phys(addr, buf, SCAN_WINDOW) < 0)
			continue;
		for (off = 0; off + 4 <= SCAN_WINDOW; off += 4096) {
			uint32_t sig = le32(buf + off);

			if (sig == RAMOOPS_SIG || sig == SEC_LOG_MAGIC) {
				printf("  hit 0x%08llx  sig 0x%08x  %s\n",
				       (unsigned long long)(addr + off), sig,
				       sig == RAMOOPS_SIG ? "ramoops/persistent_ram"
							  : "sec_log/ram_console");
				hits++;
			}
		}
	}
	free(buf);
	printf("%d hit(s)%s\n", hits,
	       hits ? "" : " - the buffer was never initialised, see docs/DEBUG-TWRP.md");
	return hits ? 0 : 2;
}

int main(int argc, char **argv)
{
	uint64_t addr = DEFAULT_ADDR;
	uint64_t size = DEFAULT_SIZE;
	const char *out = DEFAULT_OUT;
	unsigned char *buf;
	FILE *f;
	int scan = 0;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = 4096;

	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		printf("usage: memdump [<phys_addr> <size> [outfile]]\n"
		       "       memdump --scan\n"
		       "default: 0x%08llx 0x%llx %s\n",
		       (unsigned long long)DEFAULT_ADDR,
		       (unsigned long long)DEFAULT_SIZE, DEFAULT_OUT);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--scan"))
		scan = 1;
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

	if (scan)
		return do_scan();

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
			       "./memdump --scan\n", sig);
	}

	write_text(out, buf, (size_t)size);
	free(buf);
	return 0;
}
