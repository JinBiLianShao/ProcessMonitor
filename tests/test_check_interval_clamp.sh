#!/usr/bin/env bash
# check_interval = 0 必须被钳位到 1 秒，而不是退化成 100% CPU 忙等。
source "$(dirname "$0")/lib.sh" "$@"
setup

write_child sleeper.sh 'exec /bin/sleep 30'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 0

[Busy]
path = $WORK/sleeper.sh
working_dir = $WORK
EOF

CHILD_PATTERN="$WORK/sleeper.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "进程没有被启动"
sleep 4

cpu="$(ps -o %cpu= -p "$DAEMON_PID" 2>/dev/null | tr -d ' ')"
if [ -z "$cpu" ]; then
    fail "守护进程已经退出"
fi

# 钳位后应当接近 0%；旧实现在这里稳定 99%
if awk -v c="$cpu" 'BEGIN { exit !(c > 20) }'; then
    fail "check_interval=0 导致 CPU 占用 ${cpu}%（应被钳位到 1 秒）"
fi

if ! grep -q "Value out of range for check_interval" "$WORK/daemon.log"; then
    fail "钳位时没有输出告警"
fi

stop_daemon 15 || fail "守护进程没有在超时内退出"
pass "check_interval=0 被钳位，CPU 占用 ${cpu}%"
