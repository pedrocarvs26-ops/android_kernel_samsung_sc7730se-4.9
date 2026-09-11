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
 *     past the end of lowmem.
 *
 * memdump does the seek with pread() on a _FILE_OFFSET_BITS=64 build, and if
 * the kernel refuses the read it falls back to mmap(), which only has to pass
 * valid_mmap_phys_addr_range(). It also writes a plain text version of the
 * dump next to the binary, so no `strings` binary is needed in the recovery.
 *
 * Two ways this tool has already killed the tablet
 * ------------------------------------------------
 * Both were reported from the device, and both reset it instantly with no
 * output at all:
 *
 *   1. --scan, which walked 0x80000000..0xa0000000 a megabyte at a time.
 *      Parts of that range are TrustZone and modem carve-outs; a non-secure
 *      read aborts on the bus and the SoC turns the abort into a reboot.
 *   2. reading the bootloader's stock RAM console window,
 *      0x86b00000..0x86c00000 ("sec_log=0xffe00@0x86b00000" on the stock
 *      command line). The recovery kernel carves that window out of its own
 *      memory map, so the /dev/mem read() path resolves it to a virtual
 *      address that is not mapped, faults in kernel context, and the Samsung
 *      kernel is built with panic_on_oops: instant reset. However well the
 *      bootloader protects that window, it is useless to us if reading it
 *      kills the only shell we have.
 *
 * What is known to work
 * ---------------------
 * A 1 MiB pread at 0x89b00000 succeeded and came back with data (CP firmware
 * strings). The modem window, 0x88000000..0x89c00000, is reserved by the
 * stock kernel but still inside its linear map, so it is readable from
 * recovery - which is exactly why the RAM console lives at the top of it.
 * Addresses outside that proven-safe window now need --force, every read is
 * announced and flushed before it happens, and there is no blind scan.
 *
 * Build
 * -----
 *     sudo apt-get install -y gcc-arm-linux-gnueabihf libc6-dev-armhf-cross
 *     arm-linux-gnueabihf-gcc -static -O2 -Wall -o memdump tools/memdump.c
 *
 * libc6-dev-armhf-cross is not optional. The kernel is freestanding and
 * builds with the compiler alone, but this is ordinary userspace code:
 * without the armhf libc headers the Debian cross compiler ends its include
 * search in the host /usr/include, picks up the x86_64 <errno.h> and stops at
 * "sys/cdefs.h: No such file or directory". The package also carries the
 * libc.a that -static needs.
 *
 * Use (TWRP terminal, right after a warm reboot)
 * ----------------------------------------------
 *     ./memdump                      # 1 MiB at 0x89b00000 -> /sdcard/ramoops.bin
 *     ./memdump 0x89b00000 0x100000 /sdcard/ramoops.bin
 *     ./memdump --probe              # 32 bytes at each known log address
 *     ./memdump --mark               # write a marker into the zone
 *     ./memdump --check              # is the marker still there after a reboot?
 *
 * --mark/--check answer the one open question about this region: the
 * bootloader loads CP firmware into the modem window, and if it does that on
 * recovery boots as well, it would wipe our log before it can be read. Mark
 * it, reboot straight back into recovery, check it. No kernel flashing
 * needed, and nothing else in the system uses that megabyte in recovery.
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
 * The RAM console: the top megabyte of the modem window. Reserved by both
 * kernels, readable from recovery, see docs/DEBUG-TWRP.md.
 */
#define DEFAULT_ADDR 0x89b00000ULL
#define DEFAULT_SIZE 0x100000ULL
#define DEFAULT_OUT "/sdcard/ramoops.bin"

/* Proven readable from the TWRP kernel: the CP (modem) carve-out. */
#define SAFE_LO 0x88000000ULL
#define SAFE_HI 0x89c00000ULL

/*
 * The bootloader's stock RAM console window. Listed so --probe can name it,
 * never read: reading it from recovery resets the device (see above).
 */
#define SECLOG_BASE 0x86b00000ULL
#define SECLOG_SIZE 0xffe00ULL

/* PERSISTENT_RAM_SIG from fs/pstore/ram_core.c, 'DBGC' */
#define RAMOOPS_SIG 0x43474244U
/* Samsung sec_log / ram_console magic seen in the 3.10 vendor tree, 'LOGM' */
#define SEC_LOG_MAGIC 0x4d474f4cU
/* --mark writes this, --check looks for it, 'MARK' */
#define MARK_MAGIC 0x4b52414dU

static const char mark_text[] = "MARK memdump persistence test\n";

static int mem_fd = -1;
static long page_size = 4096;
static int used_mmap = 0;
static int force = 0;

/*
 * Refuse anything outside the window we have actually read from this device.
 * Every fatal accident so far was a read of an address nobody had checked.
 */
static int addr_ok(uint64_t addr, uint64_t size)
{
	if (force)
		return 1;
	if (addr >= SAFE_LO && size <= SAFE_HI - SAFE_LO &&
	    addr + size <= SAFE_HI)
		return 1;

	fprintf(stderr,
		"memdump: refusing to touch 0x%08llx + 0x%llx\n"
		"         Only 0x%08llx..0x%08llx, the modem window where the\n"
		"         RAM console lives, is known to be readable from the\n"
		"         recovery kernel. Reading the bootloader's sec_log\n"
		"         window at 0x%08llx, or anything TrustZone owns, resets\n"
		"         the tablet on the spot with no output.\n"
		"         Pass --force only if you are ready for that.\n",
		(unsigned long long)addr, (unsigned long long)size,
		(unsigned long long)SAFE_LO, (unsigned long long)SAFE_HI,
		(unsigned long long)SECLOG_BASE);
	return 0;
}

/*
 * Read len bytes of physical memory: pread() first, then mmap().
 * The announcement is printed and flushed *before* the access, so that if the
 * device resets, the last line on the screen says how far we got.
 */
static int read_phys(uint64_t phys, unsigned char *buf, size_t len)
{
	size_t done = 0;

	printf("reading  : 0x%08llx + 0x%llx\n",
	       (unsigned long long)phys, (unsigned long long)len);
	fflush(stdout);

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

/* Same idea in the other direction, used only by --mark. */
static int write_phys(uint64_t phys, const unsigned char *buf, size_t len)
{
	ssize_t n;

	printf("writing  : 0x%08llx + 0x%llx\n",
	       (unsigned long long)phys, (unsigned long long)len);
	fflush(stdout);

	n = pwrite(mem_fd, buf, len, (off_t)phys);
	if (n == (ssize_t)len)
		return 0;

	{
		uint64_t base = phys & ~((uint64_t)page_size - 1);
		size_t delta = (size_t)(phys - base);
		size_t maplen = len + delta;
		void *p;

		maplen = (maplen + (size_t)page_size - 1) &
			 ~((size_t)page_size - 1);
		p = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED,
			 mem_fd, (off_t)base);
		if (p == MAP_FAILED)
			return -1;
		memcpy((unsigned char *)p + delta, buf, len);
		msync(p, maplen, MS_SYNC);
		munmap(p, maplen);
		return 0;
	}
}

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static const char *sig_name(uint32_t sig)
{
	if (sig == RAMOOPS_SIG)
		return "'DBGC' ramoops header";
	if (sig == SEC_LOG_MAGIC)
		return "'LOGM' vendor sec_log";
	if (sig == MARK_MAGIC)
		return "'MARK' memdump marker";
	if (sig == 0)
		return "zero";
	return "unknown";
}

/* Write the printable runs of the dump to <out>.txt. */
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

/* Report the bootloader's "sec_log=SIZE@BASE" without going near it. */
static int parse_cmdline_seclog(uint64_t *base, uint64_t *size)
{
	char line[4096];
	char *p, *at;
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
	p += sizeof("sec_log=") - 1;
	at = strchr(p, '@');
	if (!at)
		return -1;
	*size = strtoull(p, NULL, 0);
	*base = strtoull(at + 1, NULL, 0);
	return 0;
}

/*
 * Look at 32 bytes in each place a log header could legitimately be, all of
 * them inside the window this device has already let us read.
 */
static int do_probe(void)
{
	struct {
		uint64_t addr;
		const char *what;
	} cand[3];
	uint64_t base = 0, size = 0;
	unsigned char b[32];
	int i, n = 0, hits = 0;

	cand[n].addr = DEFAULT_ADDR;
	cand[n++].what = "our RAM console zone";
	cand[n].addr = DEFAULT_ADDR + 0x1000;
	cand[n++].what = "one page in, in case the zone shifted";
	cand[n].addr = SAFE_LO;
	cand[n++].what = "modem window base (CP firmware if loaded)";

	if (parse_cmdline_seclog(&base, &size) == 0)
		printf("cmdline  : sec_log=0x%llx@0x%llx "
		       "(not read: reading it resets the device)\n",
		       (unsigned long long)size, (unsigned long long)base);
	else
		printf("cmdline  : no sec_log= parameter\n");

	for (i = 0; i < n; i++) {
		memset(b, 0, sizeof(b));
		if (read_phys(cand[i].addr, b, sizeof(b)) < 0) {
			printf("0x%08llx  unreadable  %-24s %s\n",
			       (unsigned long long)cand[i].addr,
			       strerror(errno), cand[i].what);
			continue;
		}
		printf("0x%08llx  0x%08x  %-24s %s\n",
		       (unsigned long long)cand[i].addr, le32(b),
		       sig_name(le32(b)), cand[i].what);
		if (le32(b) == RAMOOPS_SIG || le32(b) == SEC_LOG_MAGIC)
			hits++;
	}
	printf("%d log header(s) found\n", hits);
	return hits ? 0 : 2;
}

/*
 * --mark / --check: does this region survive a reboot into recovery?
 *
 * The bootloader loads CP firmware somewhere in 0x88000000..0x89c00000. If it
 * does that on recovery boots too, the RAM console is wiped before we can
 * read it, and no kernel change would ever fix that. Marking the zone and
 * rebooting straight back into TWRP settles it in two minutes.
 */
static int do_mark(uint64_t addr)
{
	unsigned char b[32];

	memset(b, 0, sizeof(b));
	b[0] = (unsigned char)(MARK_MAGIC & 0xff);
	b[1] = (unsigned char)((MARK_MAGIC >> 8) & 0xff);
	b[2] = (unsigned char)((MARK_MAGIC >> 16) & 0xff);
	b[3] = (unsigned char)((MARK_MAGIC >> 24) & 0xff);
	memcpy(b + 4, mark_text, sizeof(mark_text) - 1);

	if (write_phys(addr, b, sizeof(b)) < 0) {
		fprintf(stderr, "memdump: writing 0x%08llx: %s\n",
			(unsigned long long)addr, strerror(errno));
		return 1;
	}

	memset(b, 0, sizeof(b));
	if (read_phys(addr, b, sizeof(b)) < 0 || le32(b) != MARK_MAGIC) {
		fprintf(stderr, "memdump: the marker did not stick at 0x%08llx\n",
			(unsigned long long)addr);
		return 1;
	}

	printf("marker   : written at 0x%08llx\n",
	       (unsigned long long)addr);
	printf("next     : reboot straight back into recovery (TWRP -> Reboot\n"
	       "           -> Recovery), then run ./memdump --check\n");
	return 0;
}

static int do_check(uint64_t addr)
{
	unsigned char b[32];
	uint32_t sig;

	memset(b, 0, sizeof(b));
	if (read_phys(addr, b, sizeof(b)) < 0) {
		fprintf(stderr, "memdump: reading 0x%08llx: %s\n",
			(unsigned long long)addr, strerror(errno));
		return 1;
	}
	sig = le32(b);
	printf("word     : 0x%08x %s\n", sig, sig_name(sig));

	if (sig == MARK_MAGIC) {
		printf("result   : marker survived. Nothing rewrites this zone\n"
		       "           across a reboot, so the RAM console will be\n"
		       "           readable here after a failed boot.\n");
		return 0;
	}
	if (sig == RAMOOPS_SIG) {
		printf("result   : a ramoops header is here instead - the kernel\n"
		       "           got far enough to claim the zone. Dump it with\n"
		       "           ./memdump\n");
		return 0;
	}
	printf("result   : marker gone. Something (most likely the bootloader\n"
	       "           reloading CP firmware) rewrites this zone on every\n"
	       "           boot, so no RAM console here can survive.\n");
	return 2;
}

static void usage(void)
{
	printf("usage: memdump [<phys_addr> [<size> [outfile]]]\n"
	       "       memdump --probe    32 bytes at each known log address\n"
	       "       memdump --mark     write a marker into the zone\n"
	       "       memdump --check    look for the marker after a reboot\n"
	       "       memdump --force    allow addresses outside 0x%08llx..0x%08llx\n"
	       "default: 0x%08llx 0x%llx %s\n",
	       (unsigned long long)SAFE_LO, (unsigned long long)SAFE_HI,
	       (unsigned long long)DEFAULT_ADDR,
	       (unsigned long long)DEFAULT_SIZE, DEFAULT_OUT);
}

int main(int argc, char **argv)
{
	uint64_t addr = DEFAULT_ADDR;
	uint64_t size = DEFAULT_SIZE;
	const char *out = DEFAULT_OUT;
	unsigned char *buf;
	FILE *f;
	int probe = 0, mark = 0, check = 0;
	int i, pos = 0;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = 4096;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		}
		if (!strcmp(a, "--force")) {
			force = 1;
		} else if (!strcmp(a, "--probe")) {
			probe = 1;
		} else if (!strcmp(a, "--mark")) {
			mark = 1;
		} else if (!strcmp(a, "--check")) {
			check = 1;
		} else if (!strcmp(a, "--scan")) {
			fprintf(stderr,
				"memdump: --scan is gone. Walking DRAM blindly\n"
				"         crosses TrustZone and modem carve-outs,\n"
				"         and the bus abort that follows resets the\n"
				"         tablet before anything can be printed.\n"
				"         Use --probe.\n");
			return 1;
		} else if (!strcmp(a, "--seclog")) {
			fprintf(stderr,
				"memdump: --seclog is gone. Reading the\n"
				"         bootloader's window at 0x%08llx from\n"
				"         recovery resets the device: that memory is\n"
				"         carved out of the recovery kernel's own\n"
				"         map, so the read faults in kernel context\n"
				"         and panic_on_oops reboots the tablet.\n",
				(unsigned long long)SECLOG_BASE);
			return 1;
		} else if (a[0] == '-') {
			fprintf(stderr, "memdump: unknown option %s\n", a);
			usage();
			return 1;
		} else {
			if (pos == 0)
				addr = strtoull(a, NULL, 0);
			else if (pos == 1)
				size = strtoull(a, NULL, 0);
			else if (pos == 2)
				out = a;
			pos++;
		}
	}

	if (!probe && !addr_ok(addr, (mark || check) ? 32 : size))
		return 1;

	mem_fd = open("/dev/mem", (mark ? O_RDWR : O_RDONLY) | O_SYNC);
	if (mem_fd < 0) {
		fprintf(stderr, "memdump: /dev/mem: %s\n", strerror(errno));
		if (errno == ENOENT)
			fprintf(stderr, "try: mknod /dev/mem c 1 1\n");
		return 1;
	}

	if (probe)
		return do_probe();
	if (mark)
		return do_mark(addr);
	if (check)
		return do_check(addr);

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
		fprintf(stderr,
			"neither read() nor mmap() worked on this kernel\n");
		return 1;
	}

	f = fopen(out, "wb");
	if (!f) {
		fprintf(stderr, "memdump: %s: %s\n", out, strerror(errno));
		return 1;
	}
	fwrite(buf, 1, (size_t)size, f);
	fclose(f);

	printf("method   : %s\n", used_mmap ? "mmap" : "pread");
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
		else if (sig == MARK_MAGIC)
			printf("signature: 0x%08x 'MARK' - only our own marker is "
			       "here, the kernel wrote nothing\n", sig);
		else
			printf("signature: 0x%08x - no log header here; try "
			       "./memdump --probe\n", sig);
	}

	write_text(out, buf, (size_t)size);
	free(buf);
	return 0;
}
