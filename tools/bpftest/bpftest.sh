#!/system/bin/sh
# k49 eBPF smoke test driver (debug only; rm -r /data/local/tmp/k49 to disable).
#   early: full test, run by init at post-fs-data (before bpfloader and netd)
#   late:  state dump after boot (what bpfloader and netd set up)
D=/data/local/tmp/k49
B=$D/bpftest
L=/data/local/tmp/bootlog
P=/sys/fs/bpf
TC=${TC:-tc}
mode=${1:-early}
mkdir -p $L
exec > $L/bpftest-$mode.txt 2>&1
echo "== k49 bpftest $mode"
uname -a
echo "sdk=$(getprop ro.build.version.sdk) first_api=$(getprop ro.product.first_api_level) type=$(getprop ro.build.type)"
echo "selinux=$(getenforce) unpriv_bpf_disabled=$(cat /proc/sys/kernel/unprivileged_bpf_disabled)"
grep -E ' (bpf|cgroup2) ' /proc/mounts
echo "selinux bpf perms: $(ls /sys/fs/selinux/class/bpf/perms 2>&1 | tr '\n' ' ')"
ls -laZ $P

if [ "$mode" = late ]; then
	echo "== iptables (bpf)"
	iptables -w -S 2>&1 | grep -E -e '-m bpf|^-N bw_' | head -40
	ip6tables -w -S 2>&1 | grep -e '-m bpf' | head -20
	echo "== tc"
	$TC qdisc show 2>&1
	echo "== netd trafficcontroller"
	dumpsys netd trafficcontroller 2>&1 | head -150
	echo "== done"
	exit 0
fi

$B basic
echo "basic rc=$?"

echo "== xt_bpf"
if $B xt-prep; then
	iptables -w -N k49t
	iptables -w -A k49t -m bpf --object-pinned $P/k49t_xt_prog -j ACCEPT
	echo "iptables rule rc=$?"
	iptables -w -I OUTPUT -o lo -p udp --dport 40999 -j k49t
	echo "iptables jump rc=$?"
	$B xt-send
	echo "xt-send rc=$?"
	iptables -w -L k49t -v -n -x
	iptables -w -D OUTPUT -o lo -p udp --dport 40999 -j k49t
	iptables -w -F k49t
	iptables -w -X k49t
	echo "iptables cleanup rc=$?"
fi

echo "== cls_bpf"
if $B tc-prep; then
	$TC qdisc add dev lo clsact
	echo "clsact rc=$?"
	$TC filter add dev lo egress bpf da object-pinned $P/k49t_tc_prog
	echo "filter rc=$?"
	$TC filter show dev lo egress
	$B tc-send
	echo "tc-send rc=$?"
	$TC qdisc del dev lo clsact
	echo "clsact del rc=$?"
fi

$B cleanup
ls -la $P
echo "== done"
