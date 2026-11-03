#!/usr/bin/env bash
# 频繁重启时不能积累僵尸进程。
#
# 判据说明：子进程退出到 reaper 回收之间必然存在一个极短的 Z 态窗口，
# 任何守护进程都是如此。所以这里检验的是"不累积"——多次采样中僵尸数的
# 峰值必须很小，且停止后必须归零；而不是某一瞬间恰好为 0。
source "$(dirname "$0")/lib.sh" "$@"
setup

write_child fast.sh 'exit 0'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Fast]
path = $WORK/fast.sh
working_dir = $WORK
restart_delay = 0
max_restarts = -1
graceful_timeout = 1
EOF

CHILD_PATTERN="$WORK/fast.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "进程没有被启动"

peak=0
for i in $(seq 1 40); do
    n="$(ps -eo ppid=,stat= | awk -v p="$DAEMON_PID" '$1 == p && $2 ~ /^Z/ { c++ } END { print c+0 }')"
    if [ "$n" -gt "$peak" ]; then
        peak="$n"
    fi
    sleep 0.1
done

restarts="$(grep -c "Started Unix process" "$WORK/daemon.log")"
if [ "$restarts" -lt 3 ]; then
    fail "4 秒内只重启了 ${restarts} 次，样本不足"
fi
if [ "$peak" -gt 2 ]; then
    fail "僵尸进程峰值 ${peak} 个（${restarts} 次重启），说明存在累积"
fi

stop_daemon 20 || fail "守护进程没有在超时内退出"

left="$(ps -eo stat=,cmd= | awk -v p="$WORK/fast.sh" 'index($0, p) && $1 ~ /^Z/ { c++ } END { print c+0 }')"
if [ "$left" -ne 0 ]; then
    fail "停止后仍有 ${left} 个僵尸进程"
fi

pass "僵尸进程不累积（${restarts} 次重启，峰值 ${peak} 个）"
