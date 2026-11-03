#!/usr/bin/env bash
# 进程健康运行足够久之后必须重置重启预算，否则长期运行的守护进程
# 会因为历史累计次数耗尽而在某次偶发崩溃后静默停止拉起。
source "$(dirname "$0")/lib.sh" "$@"
setup

# 每次运行 3 秒后以非 0 退出；max_restarts=1 但 restart_reset_after=2，
# 因此每轮都满足"健康运行够久"，预算被重置，重启次数应当超过 1。
write_child flapper.sh 'sleep 3; exit 1'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Flapper]
path = $WORK/flapper.sh
working_dir = $WORK
restart_delay = 0
max_restarts = 1
restart_reset_after = 2
graceful_timeout = 1
EOF

CHILD_PATTERN="$WORK/flapper.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "进程没有被启动"
sleep 12
stop_daemon 20 || true

starts="$(grep -c "Started Unix process" "$WORK/daemon.log")"
if [ "$starts" -lt 3 ]; then
    fail "12 秒内只重启了 ${starts} 次；max_restarts=1 的预算没有被 reset"
fi
if ! grep -q "resetting restart budget" "$WORK/daemon.log"; then
    fail "日志里没有预算重置的记录"
fi
if grep -q "Max restarts reached" "$WORK/daemon.log"; then
    fail "预算重置后仍然判定超限"
fi

pass "健康运行后重置重启预算（12 秒内重启 ${starts} 次）"
