#!/usr/bin/env bash
# restart_delay 必须真正生效：两次重启之间的间隔不小于配置值。
# 旧版解析了 restart_delay 却从不使用，重启间隔只等于 check_interval。
source "$(dirname "$0")/lib.sh" "$@"
setup

STARTS="$WORK/starts.log"
write_child crasher.sh "date +%s%N >> $STARTS; exit 3"

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Crasher]
path = $WORK/crasher.sh
working_dir = $WORK
restart_delay = 3
restart_backoff_max = 3
max_restarts = 2
graceful_timeout = 1
EOF

CHILD_PATTERN="$WORK/crasher.sh"
start_daemon

wait_for_log "Max restarts reached" 25 || fail "没有在预期时间内耗尽重启预算"
stop_daemon 15 || true

starts="$(wc -l <"$STARTS" 2>/dev/null || echo 0)"
if [ "$starts" -lt 2 ]; then
    fail "子进程只启动了 ${starts} 次，无法测量退避间隔"
fi

gap="$(awk 'NR==1 { first=$1 } NR==2 { printf "%.3f", ($1-first)/1e9; exit }' "$STARTS")"
if awk -v g="$gap" 'BEGIN { exit !(g < 2.5) }'; then
    fail "两次启动间隔 ${gap}s，restart_delay=3 没有生效"
fi

pass "重启退避生效（间隔 ${gap}s，restart_delay=3）"
