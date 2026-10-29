// SPDX-License-Identifier: GPL-2.0
/*
 * k49 eBPF smoke test for the Linux 4.9 port on gtelwifi (SM-T560).
 *
 * Freestanding (no libc, raw ARM EABI syscalls), so one static binary runs
 * under any Android userspace.  It exercises what Android's bpfloader and
 * netd need from a 4.9 kernel:
 *   - maps: NULL-key get_next_key, pinning, BPF_F_RDONLY/WRONLY object get
 *   - verifier rejection + log
 *   - socket filter (SO_ATTACH_BPF), ctx access, cookie/uid helpers
 *   - cgroup v2 BPF_PROG_TYPE_CGROUP_SKB ingress/egress with
 *     bpf_get_socket_uid/cookie, bpf_skb_load_bytes, BPF_F_ALLOW_MULTI
 *     across a two-level hierarchy, detach
 *   - sk_uid following fchown() on the socket, SO_COOKIE
 *   - pinned programs for xt_bpf (--object-pinned) and cls_bpf
 *     (the iptables/tc parts are driven from bpftest.sh)
 *
 * Usage: bpftest basic | xt-prep | xt-send | tc-prep | tc-send | cleanup
 */
#include <linux/types.h>
#include <linux/bpf.h>
#include <linux/in.h>
#include <linux/pkt_cls.h>
#include <asm/unistd.h>
#include <asm/socket.h>
#include <asm/fcntl.h>

_Static_assert(BPF_FUNC_get_socket_cookie == 46, "helper id");
_Static_assert(BPF_FUNC_get_socket_uid == 47, "helper id");
_Static_assert(SO_COOKIE == 57, "SO_COOKIE");

#define AF_INET		2
#define SOCK_DGRAM	2
#define SOCK_CLOEXEC	02000000
#define AT_FDCWD	(-100)
#define AT_REMOVEDIR	0x200
#define OVERFLOWUID	65534

#define PIN_DIR		"/sys/fs/bpf/"
#define XT_MAP		PIN_DIR "k49t_xt_map"
#define XT_PROG		PIN_DIR "k49t_xt_prog"
#define TC_MAP		PIN_DIR "k49t_tc_map"
#define TC_PROG		PIN_DIR "k49t_tc_prog"
#define T_MAP		PIN_DIR "k49t_map"
#define CG2		"/data/local/tmp/k49/cg2"

#define XT_PORT		40999
#define TC_PORT		40998
#define LO_PORT		40997

/* ---------------------------------------------------------------- runtime */

#if defined(__x86_64__)
/* x86_64 build is only used for qemu runs of the same tree */
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	long ret;

	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8),
			   "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}
#else
static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;
	register long r7 __asm__("r7") = n;

	__asm__ volatile("svc #0"
			 : "+r"(r0)
			 : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
			   "r"(r7)
			 : "memory");
	return r0;
}
#endif

void *memset(void *d, int c, unsigned long n)
{
	volatile unsigned char *p = d;

	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	volatile unsigned char *p = d;
	const unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

static unsigned long slen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static int seq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void __attribute__((noreturn)) sys_exit(int code)
{
	for (;;)
		sc(__NR_exit_group, code, 0, 0, 0, 0, 0);
}

/* libgcc's divide-by-zero and unwinder paths reference these */
int raise(int sig)
{
	sys_exit(128 + sig);
}

void __attribute__((noreturn)) abort(void)
{
	sys_exit(134);
}

static void out(const char *s)
{
	sc(__NR_write, 1, (long)s, slen(s), 0, 0, 0);
}

static void out_u64(unsigned long long v)
{
	char b[24];
	int i = 23;

	b[i] = 0;
	do {
		b[--i] = '0' + (char)(v % 10);
		v /= 10;
	} while (v);
	out(b + i);
}

static void __attribute__((unused)) out_hex(unsigned long long v)
{
	char b[20];
	int i = 19;

	b[i] = 0;
	do {
		b[--i] = "0123456789abcdef"[v & 15];
		v >>= 4;
	} while (v);
	b[--i] = 'x';
	b[--i] = '0';
	out(b + i);
}

static const char *ename(long e)
{
	switch (-e) {
	case 1: return "EPERM";
	case 2: return "ENOENT";
	case 7: return "E2BIG";
	case 9: return "EBADF";
	case 11: return "EAGAIN";
	case 12: return "ENOMEM";
	case 13: return "EACCES";
	case 14: return "EFAULT";
	case 16: return "EBUSY";
	case 17: return "EEXIST";
	case 22: return "EINVAL";
	case 28: return "ENOSPC";
	case 38: return "ENOSYS";
	case 95: return "EOPNOTSUPP";
	case 99: return "EADDRNOTAVAIL";
	case 100: return "ENETDOWN";
	case 101: return "ENETUNREACH";
	case 111: return "ECONNREFUSED";
	default: return "E?";
	}
}

/* prints "<v>" for v >= 0 and "-<errno> (ENAME)" otherwise */
static void out_ret(long v)
{
	if (v >= 0) {
		out_u64((unsigned long)v);
		return;
	}
	out("-");
	out_u64((unsigned long)-v);
	out(" (");
	out(ename(v));
	out(")");
}

static int n_pass, n_fail;

static void result(const char *name, int ok)
{
	out("RESULT ");
	out(name);
	out(ok ? " PASS\n" : " FAIL\n");
	if (ok)
		n_pass++;
	else
		n_fail++;
}

/* RESULT for "got == want" with both values printed */
static void check_eq(const char *name, unsigned long long got,
		     unsigned long long want)
{
	out("  ");
	out(name);
	out(": got ");
	out_u64(got);
	out(" want ");
	out_u64(want);
	out("\n");
	result(name, got == want);
}

/* RESULT for a syscall return value that must equal want (0 or -errno) */
static void check_ret(const char *name, long got, long want)
{
	out("  ");
	out(name);
	out(": ret ");
	out_ret(got);
	out(" want ");
	out_ret(want);
	out("\n");
	result(name, got == want);
}

/* RESULT for a syscall that must succeed (>= 0) */
static int check_ok(const char *name, long got)
{
	out("  ");
	out(name);
	out(": ret ");
	out_ret(got);
	out("\n");
	result(name, got >= 0);
	return got >= 0;
}

static long sys_close(int fd)
{
	return sc(__NR_close, fd, 0, 0, 0, 0, 0);
}

static long sys_open(const char *p, int flags, int mode)
{
	return sc(__NR_openat, AT_FDCWD, (long)p, flags | O_CLOEXEC, mode,
		  0, 0);
}

static long sys_unlink(const char *p)
{
	return sc(__NR_unlinkat, AT_FDCWD, (long)p, 0, 0, 0, 0);
}

static long sys_rmdir(const char *p)
{
	return sc(__NR_unlinkat, AT_FDCWD, (long)p, AT_REMOVEDIR, 0, 0, 0);
}

static long sys_mkdir(const char *p, int mode)
{
	return sc(__NR_mkdirat, AT_FDCWD, (long)p, mode, 0, 0, 0);
}

static long write_str(const char *path, const char *s)
{
	long fd = sys_open(path, O_WRONLY, 0), r;

	if (fd < 0)
		return fd;
	r = sc(__NR_write, fd, (long)s, slen(s), 0, 0, 0);
	sys_close(fd);
	return r < 0 ? r : 0;
}

static void utoa(unsigned long v, char *b)
{
	char t[12];
	int i = 0, j = 0;

	do {
		t[i++] = '0' + (char)(v % 10);
		v /= 10;
	} while (v);
	while (i)
		b[j++] = t[--i];
	b[j++] = '\n';
	b[j] = 0;
}

/* ------------------------------------------------------------- bpf calls */

static long sys_bpf(int cmd, union bpf_attr *a)
{
	return sc(__NR_bpf, cmd, (long)a, sizeof(*a), 0, 0, 0);
}

static long map_create(__u32 type, __u32 ks, __u32 vs, __u32 max, __u32 fl)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.map_type = type;
	a.key_size = ks;
	a.value_size = vs;
	a.max_entries = max;
	a.map_flags = fl;
	return sys_bpf(BPF_MAP_CREATE, &a);
}

static long map_op(int cmd, int fd, const void *key, void *val, __u64 fl)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.map_fd = fd;
	a.key = (unsigned long)key;
	a.value = (unsigned long)val;
	a.flags = fl;
	return sys_bpf(cmd, &a);
}

static long map_get_next(int fd, const void *key, void *next)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.map_fd = fd;
	a.key = (unsigned long)key;
	a.next_key = (unsigned long)next;
	return sys_bpf(BPF_MAP_GET_NEXT_KEY, &a);
}

static __u64 map_get(int fd, __u32 key)
{
	__u64 v = 0;

	map_op(BPF_MAP_LOOKUP_ELEM, fd, &key, &v, 0);
	return v;
}

static long obj_pin(int fd, const char *path)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.pathname = (unsigned long)path;
	a.bpf_fd = fd;
	return sys_bpf(BPF_OBJ_PIN, &a);
}

static long obj_get(const char *path, __u32 flags)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.pathname = (unsigned long)path;
	a.file_flags = flags;
	return sys_bpf(BPF_OBJ_GET, &a);
}

static char logbuf[65536];

static long prog_load_log(__u32 type, const struct bpf_insn *insns, int cnt,
			  int level)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.prog_type = type;
	a.insns = (unsigned long)insns;
	a.insn_cnt = cnt;
	a.license = (unsigned long)"GPL";
	if (level) {
		a.log_buf = (unsigned long)logbuf;
		a.log_size = sizeof(logbuf);
		a.log_level = level;
		logbuf[0] = 0;
	}
	return sys_bpf(BPF_PROG_LOAD, &a);
}

/* print the last few lines of the verifier log */
static void out_log_tail(void)
{
	unsigned long n = slen(logbuf), i, lines = 0;

	for (i = n; i > 0; i--)
		if (logbuf[i - 1] == '\n' && ++lines > 6)
			break;
	out("  verifier log tail:\n");
	out(logbuf + i);
	if (n && logbuf[n - 1] != '\n')
		out("\n");
}

static long prog_load(const char *name, __u32 type,
		      const struct bpf_insn *insns, int cnt)
{
	long fd = prog_load_log(type, insns, cnt, 0);

	if (fd < 0) {
		out("  load ");
		out(name);
		out(" failed: ");
		out_ret(fd);
		out("\n");
		prog_load_log(type, insns, cnt, 1);
		out_log_tail();
	}
	return fd;
}

static long prog_attach(int target, int prog, __u32 type, __u32 flags)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.target_fd = target;
	a.attach_bpf_fd = prog;
	a.attach_type = type;
	a.attach_flags = flags;
	return sys_bpf(BPF_PROG_ATTACH, &a);
}

static long prog_detach(int target, int prog, __u32 type)
{
	union bpf_attr a;

	memset(&a, 0, sizeof(a));
	a.target_fd = target;
	a.attach_bpf_fd = prog;
	a.attach_type = type;
	return sys_bpf(BPF_PROG_DETACH, &a);
}

/* ---------------------------------------------------------- insn macros */

#define INSN(c, d, s, o, i)						\
	((struct bpf_insn){ .code = (c), .dst_reg = (d), .src_reg = (s), \
			    .off = (o), .imm = (i) })
#define MOV64_REG(d, s)		INSN(BPF_ALU64 | BPF_MOV | BPF_X, d, s, 0, 0)
#define MOV64_IMM(d, i)		INSN(BPF_ALU64 | BPF_MOV | BPF_K, d, 0, 0, i)
#define ADD64_IMM(d, i)		INSN(BPF_ALU64 | BPF_ADD | BPF_K, d, 0, 0, i)
#define LDX(sz, d, s, o)	INSN(BPF_LDX | (sz) | BPF_MEM, d, s, o, 0)
#define STX(sz, d, s, o)	INSN(BPF_STX | (sz) | BPF_MEM, d, s, o, 0)
#define ST(sz, d, o, i)		INSN(BPF_ST | (sz) | BPF_MEM, d, 0, o, i)
#define XADD(sz, d, s, o)	INSN(BPF_STX | (sz) | BPF_XADD, d, s, o, 0)
#define JEQ_IMM(d, i, o)	INSN(BPF_JMP | BPF_JEQ | BPF_K, d, 0, o, i)
#define CALL(f)			INSN(BPF_JMP | BPF_CALL, 0, 0, 0, f)
#define EXIT()			INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

#define R0 BPF_REG_0
#define R1 BPF_REG_1
#define R2 BPF_REG_2
#define R3 BPF_REG_3
#define R4 BPF_REG_4
#define R6 BPF_REG_6
#define R7 BPF_REG_7
#define R10 BPF_REG_10

#define SKB_OFF(f)	((short)__builtin_offsetof(struct __sk_buff, f))

/* tiny program builder */
struct pb {
	struct bpf_insn i[256];
	int n;
	int mapfd;
};

static void emit(struct pb *p, struct bpf_insn insn)
{
	if (p->n < 256)
		p->i[p->n++] = insn;
}

static void emit_ld_map(struct pb *p, int reg)
{
	emit(p, INSN(BPF_LD | BPF_DW | BPF_IMM, reg, BPF_PSEUDO_MAP_FD, 0,
		     p->mapfd));
	emit(p, INSN(0, 0, 0, 0, 0));
}

/* map[key] = r0 (u64), via the stack (key at fp-12, value at fp-8) */
static void emit_store_r0(struct pb *p, int key)
{
	emit(p, STX(BPF_DW, R10, R0, -8));
	emit(p, ST(BPF_W, R10, -12, key));
	emit_ld_map(p, R1);
	emit(p, MOV64_REG(R2, R10));
	emit(p, ADD64_IMM(R2, -12));
	emit(p, MOV64_REG(R3, R10));
	emit(p, ADD64_IMM(R3, -8));
	emit(p, MOV64_IMM(R4, BPF_ANY));
	emit(p, CALL(BPF_FUNC_map_update_elem));
}

/* map[key] += r7 (atomic) */
static void emit_xadd_r7(struct pb *p, int key)
{
	emit(p, ST(BPF_W, R10, -12, key));
	emit_ld_map(p, R1);
	emit(p, MOV64_REG(R2, R10));
	emit(p, ADD64_IMM(R2, -12));
	emit(p, CALL(BPF_FUNC_map_lookup_elem));
	emit(p, JEQ_IMM(R0, 0, 1));
	emit(p, XADD(BPF_DW, R0, R7, 0));
}

/*
 * Accounting program in the style of netd's: records socket uid, cookie,
 * packet count/bytes and (optionally) the IP protocol byte read with
 * bpf_skb_load_bytes() into map[base + 0..5], then returns ret.
 */
static void build_acct(struct pb *p, int base, int load_off, int ret)
{
	p->n = 0;
	emit(p, MOV64_REG(R6, R1));
	emit(p, CALL(BPF_FUNC_get_socket_uid));
	emit_store_r0(p, base + 0);
	emit(p, MOV64_REG(R1, R6));
	emit(p, CALL(BPF_FUNC_get_socket_cookie));
	emit_store_r0(p, base + 1);
	emit(p, MOV64_IMM(R7, 1));
	emit_xadd_r7(p, base + 2);
	emit(p, LDX(BPF_W, R7, R6, SKB_OFF(len)));
	emit_xadd_r7(p, base + 3);
	if (load_off >= 0) {
		emit(p, ST(BPF_DW, R10, -24, 0));
		emit(p, MOV64_REG(R1, R6));
		emit(p, MOV64_IMM(R2, load_off));
		emit(p, MOV64_REG(R3, R10));
		emit(p, ADD64_IMM(R3, -24));
		emit(p, MOV64_IMM(R4, 1));
		emit(p, CALL(BPF_FUNC_skb_load_bytes));
		emit(p, LDX(BPF_B, R0, R10, -24));
		emit_store_r0(p, base + 4);
	}
	emit(p, LDX(BPF_W, R0, R6, SKB_OFF(protocol)));
	emit_store_r0(p, base + 5);
	emit(p, LDX(BPF_W, R0, R6, SKB_OFF(pkt_type)));
	emit_store_r0(p, base + 6);
	if (ret < 0)
		emit(p, LDX(BPF_W, R0, R6, SKB_OFF(len)));
	else
		emit(p, MOV64_IMM(R0, ret));
	emit(p, EXIT());
}

/* ------------------------------------------------------------- sockets */

static __u16 hs(__u16 v)
{
	return (__u16)((v << 8) | (v >> 8));
}

static long udp_sock(void)
{
	return sc(__NR_socket, AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0, 0, 0, 0);
}

static void lo_addr(struct sockaddr_in *sa, int port)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = hs((__u16)port);
	sa->sin_addr.s_addr = 0x0100007f;	/* 127.0.0.1, LE host */
}

static long udp_bind(int fd, int port)
{
	struct sockaddr_in sa;

	lo_addr(&sa, port);
	return sc(__NR_bind, fd, (long)&sa, sizeof(sa), 0, 0, 0);
}

/* report the first failing sendto/recvfrom so packet FAILs explain themselves */
static void io_err(const char *what, long r, int *seen)
{
	if (r >= 0 || *seen)
		return;
	*seen = 1;
	out("  info: ");
	out(what);
	out(" failed: ");
	out_ret(r);
	out("\n");
}

static int send_err_seen, recv_err_seen;

static long udp_send(int fd, int port, const char *msg)
{
	struct sockaddr_in sa;
	long r;

	lo_addr(&sa, port);
	r = sc(__NR_sendto, fd, (long)msg, slen(msg), 0, (long)&sa,
	       sizeof(sa));
	io_err("sendto", r, &send_err_seen);
	return r;
}

static long udp_recv(int fd, char *buf, int len)
{
	long r = sc(__NR_recvfrom, fd, (long)buf, len, 0, 0, 0);

	io_err("recvfrom", r, &recv_err_seen);
	return r;
}

static long set_rcvtimeo(int fd, int sec)
{
	long tv[2] = { sec, 0 };

	return sc(__NR_setsockopt, fd, SOL_SOCKET, SO_RCVTIMEO, (long)tv,
		  sizeof(tv), 0);
}

static __u64 so_cookie(int fd)
{
	__u64 c = 0;
	int len = sizeof(c);

	if (sc(__NR_getsockopt, fd, SOL_SOCKET, SO_COOKIE, (long)&c,
	       (long)&len, 0) < 0)
		return 0;
	return c;
}

static long sys_fchown(int fd, int uid)
{
#ifdef __NR_fchown32
	return sc(__NR_fchown32, fd, uid, -1, 0, 0, 0);
#else
	return sc(__NR_fchown, fd, uid, -1, 0, 0, 0);
#endif
}

/* ---------------------------------------------------------------- tests */

static void t_maps(void)
{
	static const struct { __u32 t, ks, vs; const char *n; } types[] = {
		{ 1, 4, 8, "HASH" }, { 2, 4, 8, "ARRAY" },
		{ 3, 4, 4, "PROG_ARRAY" }, { 4, 4, 4, "PERF_EVENT_ARRAY" },
		{ 5, 4, 8, "PERCPU_HASH" }, { 6, 4, 8, "PERCPU_ARRAY" },
		{ 7, 4, 64, "STACK_TRACE" }, { 8, 4, 4, "CGROUP_ARRAY" },
		{ 9, 4, 8, "LRU_HASH" }, { 10, 4, 8, "LRU_PERCPU_HASH" },
	};
	__u32 k = 7, nk = 0;
	__u64 v = 42, got = 0;
	long fd, rfd, wfd, r;
	unsigned int i;

	out("== maps\n");
	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		fd = map_create(types[i].t, types[i].ks, types[i].vs, 4, 0);
		out("  info: map type ");
		out(types[i].n);
		out(fd >= 0 ? " supported\n" : " not supported: ");
		if (fd < 0) {
			out_ret(fd);
			out("\n");
		} else {
			sys_close(fd);
		}
	}
	fd = map_create(BPF_MAP_TYPE_HASH, 4, 8, 4, BPF_F_NO_PREALLOC);
	out("  info: HASH with BPF_F_NO_PREALLOC: ");
	out_ret(fd);
	out("\n");
	if (fd >= 0)
		sys_close(fd);

	fd = map_create(BPF_MAP_TYPE_HASH, 4, 8, 8, 0);
	if (!check_ok("map_create_hash", fd))
		return;
	check_ret("map_update", map_op(BPF_MAP_UPDATE_ELEM, fd, &k, &v,
				       BPF_ANY), 0);
	check_ret("map_update_noexist_on_existing",
		  map_op(BPF_MAP_UPDATE_ELEM, fd, &k, &v, BPF_NOEXIST),
		  -17);
	map_op(BPF_MAP_LOOKUP_ELEM, fd, &k, &got, 0);
	check_eq("map_lookup", got, 42);
	r = map_get_next(fd, (void *)0, &nk);
	check_ret("map_get_next_key_null", r, 0);
	check_eq("map_get_next_key_null_value", nk, 7);
	check_ret("map_get_next_key_last", map_get_next(fd, &k, &nk), -2);

	sys_unlink(T_MAP);
	check_ret("obj_pin_map", obj_pin(fd, T_MAP), 0);
	rfd = obj_get(T_MAP, BPF_F_RDONLY);
	if (check_ok("obj_get_rdonly", rfd)) {
		got = 0;
		check_ret("rdonly_lookup", map_op(BPF_MAP_LOOKUP_ELEM, rfd, &k,
						  &got, 0), 0);
		check_ret("rdonly_update_denied",
			  map_op(BPF_MAP_UPDATE_ELEM, rfd, &k, &v, BPF_ANY),
			  -1);
		sys_close(rfd);
	}
	wfd = obj_get(T_MAP, BPF_F_WRONLY);
	if (check_ok("obj_get_wronly", wfd)) {
		check_ret("wronly_lookup_denied",
			  map_op(BPF_MAP_LOOKUP_ELEM, wfd, &k, &got, 0), -1);
		check_ret("wronly_update", map_op(BPF_MAP_UPDATE_ELEM, wfd, &k,
						  &v, BPF_ANY), 0);
		sys_close(wfd);
	}
	check_ret("obj_get_bad_flags", obj_get(T_MAP, 1), -22);
	sys_unlink(T_MAP);
	sys_close(fd);
}

static void t_verifier(void)
{
	struct bpf_insn bad[] = { EXIT() };
	struct bpf_insn sf_lb[] = {
		MOV64_IMM(R2, 0), MOV64_REG(R3, R10), ADD64_IMM(R3, -8),
		MOV64_IMM(R4, 1), CALL(BPF_FUNC_skb_load_bytes),
		MOV64_IMM(R0, 0), EXIT(),
	};
	long fd;

	out("== verifier\n");
	fd = prog_load_log(BPF_PROG_TYPE_SOCKET_FILTER, bad, 1, 1);
	check_ret("verifier_rejects_uninit_r0", fd, -13);
	out_log_tail();
	fd = prog_load_log(BPF_PROG_TYPE_SOCKET_FILTER, sf_lb, 7, 0);
	out("  info: SOCKET_FILTER calling skb_load_bytes: ");
	out_ret(fd);
	out(" (4.9: not offered to socket filters)\n");
	if (fd >= 0)
		sys_close(fd);
}

static void t_sockfilter(void)
{
	static struct pb p;
	char buf[64];
	long map, prog, rx, tx, r;
	int i, okrecv = 0;

	out("== socket filter (SO_ATTACH_BPF)\n");
	map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 16, 0);
	if (!check_ok("sf_map_create", map))
		return;
	p.mapfd = map;
	build_acct(&p, 0, -1, -1);
	prog = prog_load("sockfilter", BPF_PROG_TYPE_SOCKET_FILTER, p.i, p.n);
	if (!check_ok("sf_prog_load", prog))
		goto out_map;
	rx = udp_sock();
	tx = udp_sock();
	set_rcvtimeo(rx, 2);
	check_ret("sf_bind", udp_bind(rx, LO_PORT), 0);
	r = sc(__NR_setsockopt, rx, SOL_SOCKET, SO_ATTACH_BPF, (long)&prog,
	       sizeof(int), 0);
	check_ret("sf_attach", r, 0);
	for (i = 0; i < 3; i++) {
		udp_send(tx, LO_PORT, "K49!sockfilter");
		if (udp_recv(rx, buf, sizeof(buf)) == 14)
			okrecv++;
	}
	check_eq("sf_packets_received", okrecv, 3);
	check_eq("sf_prog_runs", map_get(map, 2), 3);
	check_eq("sf_prog_bytes", map_get(map, 3), 3 * (8 + 14));
	out("  info: rx-side uid ");
	out_u64(map_get(map, 0));
	out(" cookie ");
	out_u64(map_get(map, 1));
	out(" (skb->sk is NULL here on 4.9 -> overflowuid/0)\n");
	check_eq("sf_ctx_protocol_ip", map_get(map, 5), hs(0x0800));
	sys_close(rx);
	sys_close(tx);
	sys_close(prog);
out_map:
	sys_close(map);
}

static void t_cookie(void)
{
	long a = udp_sock(), b = udp_sock();
	__u64 ca = so_cookie(a), cb = so_cookie(b);

	out("== SO_COOKIE\n");
	out("  cookies ");
	out_u64(ca);
	out(" ");
	out_u64(cb);
	out("\n");
	result("so_cookie_nonzero_distinct", ca && cb && ca != cb);
	result("so_cookie_stable", so_cookie(a) == ca);
	sys_close(a);
	sys_close(b);
}

static long cg_move_self(const char *dir)
{
	char path[128], pid[16];
	unsigned long n = slen(dir);

	memcpy(path, dir, n);
	memcpy(path + n, "/cgroup.procs", 14);
	utoa((unsigned long)sc(__NR_getpid, 0, 0, 0, 0, 0, 0), pid);
	return write_str(path, pid);
}

static void t_cgroup(void)
{
	static struct pb pe, pi, pp;
	char buf[64];
	long r, map, pe_fd, pi_fd, pp_fd, cga, cgb, rx, tx;
	__u64 ctx, crx;
	int i, got = 0, mounted = 0;

	out("== cgroup v2 + BPF_PROG_TYPE_CGROUP_SKB\n");
	sys_mkdir("/data/local/tmp/k49", 0755);
	sys_mkdir(CG2, 0755);
	r = sc(__NR_mount, (long)"none", (long)CG2, (long)"cgroup2", 0, 0, 0);
	if (!check_ok("cg2_mount", r))
		return;
	mounted = 1;
	sys_mkdir(CG2 "/k49a", 0755);
	sys_mkdir(CG2 "/k49a/k49b", 0755);
	cga = sys_open(CG2 "/k49a", O_RDONLY | O_DIRECTORY, 0);
	cgb = sys_open(CG2 "/k49a/k49b", O_RDONLY | O_DIRECTORY, 0);
	if (!check_ok("cg2_open_a", cga) || !check_ok("cg2_open_b", cgb))
		goto out_umount;
	if (!check_ok("cg2_move_self", cg_move_self(CG2 "/k49a/k49b")))
		goto out_umount;

	map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 32, 0);
	if (!check_ok("cg_map_create", map))
		goto out_back;
	pe.mapfd = pi.mapfd = pp.mapfd = map;
	/* skb->data is at the IP header: protocol byte at offset 9 */
	build_acct(&pe, 0, 9, 1);	/* egress, child */
	build_acct(&pi, 8, 9, 1);	/* ingress, child */
	build_acct(&pp, 16, 9, 1);	/* egress, parent */
	pe_fd = prog_load("cg_egress", BPF_PROG_TYPE_CGROUP_SKB, pe.i, pe.n);
	pi_fd = prog_load("cg_ingress", BPF_PROG_TYPE_CGROUP_SKB, pi.i, pi.n);
	pp_fd = prog_load("cg_parent", BPF_PROG_TYPE_CGROUP_SKB, pp.i, pp.n);
	if (!check_ok("cg_prog_load_egress", pe_fd) ||
	    !check_ok("cg_prog_load_ingress", pi_fd) ||
	    !check_ok("cg_prog_load_parent", pp_fd))
		goto out_map;

	check_ret("cg_attach_parent_multi",
		  prog_attach(cga, pp_fd, BPF_CGROUP_INET_EGRESS,
			      BPF_F_ALLOW_MULTI), 0);
	check_ret("cg_attach_child_egress_multi",
		  prog_attach(cgb, pe_fd, BPF_CGROUP_INET_EGRESS,
			      BPF_F_ALLOW_MULTI), 0);
	check_ret("cg_attach_child_ingress",
		  prog_attach(cgb, pi_fd, BPF_CGROUP_INET_INGRESS, 0), 0);
	check_ret("cg_attach_same_prog_twice_rejected",
		  prog_attach(cgb, pe_fd, BPF_CGROUP_INET_EGRESS,
			      BPF_F_ALLOW_MULTI), -22);
	check_ret("cg_attach_nonmulti_over_multi_rejected",
		  prog_attach(cgb, pp_fd, BPF_CGROUP_INET_EGRESS, 0), -1);

	/* sockets are tied to the cgroup of their creator: create them now */
	rx = udp_sock();
	tx = udp_sock();
	set_rcvtimeo(rx, 2);
	udp_bind(rx, LO_PORT + 10);
	check_ret("fchown_tx_4321", sys_fchown(tx, 4321), 0);
	check_ret("fchown_rx_4322", sys_fchown(rx, 4322), 0);
	ctx = so_cookie(tx);
	crx = so_cookie(rx);
	for (i = 0; i < 5; i++) {
		udp_send(tx, LO_PORT + 10, "K49!cgroupskb");
		if (udp_recv(rx, buf, sizeof(buf)) == 13)
			got++;
	}
	check_eq("cg_packets_received", got, 5);
	check_eq("cg_egress_runs", map_get(map, 2), 5);
	check_eq("cg_egress_bytes", map_get(map, 3), 5 * (20 + 8 + 13));
	check_eq("cg_egress_uid_follows_fchown", map_get(map, 0), 4321);
	check_eq("cg_egress_cookie_eq_SO_COOKIE", map_get(map, 1), ctx);
	check_eq("cg_egress_load_bytes_proto_udp", map_get(map, 4), 17);
	check_eq("cg_parent_multi_runs", map_get(map, 18), 5);
	check_eq("cg_parent_uid", map_get(map, 16), 4321);
	check_eq("cg_ingress_runs", map_get(map, 10), 5);
	check_eq("cg_ingress_uid_of_rx", map_get(map, 8), 4322);
	check_eq("cg_ingress_cookie_eq_SO_COOKIE", map_get(map, 9), crx);
	check_eq("cg_ingress_load_bytes_proto_udp", map_get(map, 12), 17);
	check_eq("cg_ingress_ctx_protocol_ip", map_get(map, 13), hs(0x0800));

	/* detach the child egress program: only the parent one keeps running */
	check_ret("cg_detach_child_egress",
		  prog_detach(cgb, pe_fd, BPF_CGROUP_INET_EGRESS), 0);
	udp_send(tx, LO_PORT + 10, "K49!cgroupskb");
	udp_recv(rx, buf, sizeof(buf));
	check_eq("cg_child_egress_stopped", map_get(map, 2), 5);
	check_eq("cg_parent_still_runs", map_get(map, 18), 6);
	check_ret("cg_detach_parent",
		  prog_detach(cga, pp_fd, BPF_CGROUP_INET_EGRESS), 0);
	check_ret("cg_detach_ingress",
		  prog_detach(cgb, pi_fd, BPF_CGROUP_INET_INGRESS), 0);
	check_ret("cg_detach_again_enoent",
		  prog_detach(cgb, pi_fd, BPF_CGROUP_INET_INGRESS), -2);
	sys_close(rx);
	sys_close(tx);
out_map:
	if (pe_fd >= 0)
		sys_close(pe_fd);
	if (pi_fd >= 0)
		sys_close(pi_fd);
	if (pp_fd >= 0)
		sys_close(pp_fd);
	sys_close(map);
out_back:
	cg_move_self(CG2);
out_umount:
	if (cga >= 0)
		sys_close(cga);
	if (cgb >= 0)
		sys_close(cgb);
	sys_rmdir(CG2 "/k49a/k49b");
	sys_rmdir(CG2 "/k49a");
	if (mounted)
		sc(__NR_umount2, (long)CG2, 0, 0, 0, 0, 0);
}

/* pinned map + program for iptables -m bpf --object-pinned / tc */
static int prep_pinned(const char *mpath, const char *ppath, __u32 type,
		       int load_off, int ret)
{
	static struct pb p;
	long map, prog;

	sys_unlink(mpath);
	sys_unlink(ppath);
	map = map_create(BPF_MAP_TYPE_ARRAY, 4, 8, 16, 0);
	if (!check_ok("pin_map_create", map))
		return 1;
	p.mapfd = map;
	build_acct(&p, 0, load_off, ret);
	prog = prog_load(ppath, type, p.i, p.n);
	if (!check_ok("pin_prog_load", prog))
		return 1;
	if (!check_ok("pin_map", obj_pin(map, mpath)) ||
	    !check_ok("pin_prog", obj_pin(prog, ppath)))
		return 1;
	check_ok("obj_get_prog_rdonly", obj_get(ppath, BPF_F_RDONLY));
	return 0;
}

static int send_and_check(const char *mpath, int port, const char *pfx,
			  int exact)
{
	char name[64];
	long map = obj_get(mpath, BPF_F_RDONLY), tx;
	__u64 before, after;
	unsigned long n = slen(pfx);
	int i;

	if (!check_ok("obj_get_pinned_map", map))
		return 1;
	before = map_get(map, 2);
	tx = udp_sock();
	sys_fchown(tx, 5555);
	for (i = 0; i < 4; i++)
		udp_send(tx, port, "K49!pinned");
	after = map_get(map, 2);
	memcpy(name, pfx, n);
	if (exact) {
		memcpy(name + n, "_runs", 6);
		check_eq(name, after - before, 4);
		memcpy(name + n, "_uid", 5);
		check_eq(name, map_get(map, 0), 5555);
		memcpy(name + n, "_cookie", 8);
		check_eq(name, map_get(map, 1), so_cookie(tx));
	} else {
		out("  runs: ");
		out_u64(after - before);
		out(" (>= 4 expected)\n");
		memcpy(name + n, "_runs_ge4", 10);
		result(name, after - before >= 4);
	}
	sys_close(tx);
	sys_close(map);
	return 0;
}

static int summary(void)
{
	out("SUMMARY pass=");
	out_u64(n_pass);
	out(" fail=");
	out_u64(n_fail);
	out("\n");
	return n_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "basic";

	if (seq(cmd, "basic")) {
		t_maps();
		t_verifier();
		t_cookie();
		t_sockfilter();
		t_cgroup();
	} else if (seq(cmd, "xt-prep")) {
		if (prep_pinned(XT_MAP, XT_PROG, BPF_PROG_TYPE_SOCKET_FILTER,
				-1, 1))
			return summary() | 2;
	} else if (seq(cmd, "xt-send")) {
		send_and_check(XT_MAP, XT_PORT, "xt_bpf", 1);
	} else if (seq(cmd, "tc-prep")) {
		if (prep_pinned(TC_MAP, TC_PROG, BPF_PROG_TYPE_SCHED_CLS, 23,
				TC_ACT_OK))
			return summary() | 2;
	} else if (seq(cmd, "tc-send")) {
		send_and_check(TC_MAP, TC_PORT, "cls_bpf", 0);
	} else if (seq(cmd, "cleanup")) {
		sys_unlink(XT_MAP);
		sys_unlink(XT_PROG);
		sys_unlink(TC_MAP);
		sys_unlink(TC_PROG);
		sys_unlink(T_MAP);
		return 0;
	} else {
		out("usage: bpftest basic|xt-prep|xt-send|tc-prep|tc-send|cleanup\n");
		return 2;
	}
	return summary();
}

__attribute__((used, noreturn)) void k49_cstart(long *sp)
{
	sys_exit(main((int)sp[0], (char **)(sp + 1)));
}

#if defined(__x86_64__)
__asm__(".text\n"
	".global _start\n"
	".type _start, @function\n"
	"_start:\n"
	"	xor	%ebp, %ebp\n"
	"	mov	%rsp, %rdi\n"
	"	and	$-16, %rsp\n"
	"	call	k49_cstart\n"
	"1:	hlt\n"
	"	jmp	1b\n");
#else
__asm__(".text\n"
	".global _start\n"
	".type _start, %function\n"
	"_start:\n"
	"	mov	fp, #0\n"
	"	mov	lr, #0\n"
	"	mov	r0, sp\n"
	"	bic	sp, sp, #7\n"
	"	bl	k49_cstart\n"
	"1:	b	1b\n");
#endif
