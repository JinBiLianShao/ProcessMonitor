#!/usr/bin/env bash
# 停止时必须先发 SIGTERM 并等待 graceful_timeout。
# 旧版 stopAll() 直接 force=true，子进程根本收不到 SIGTERM。
source "$(dirname "$0")/lib.sh" "$@"
setup

cat >"$WORK/graceful.sh" <<EOF
#!/bin/sh
trap 'echo TERM > $WORK/term.flag; exit 0' TERM
while true; do sleep 0.1; done
EOF
chmod +x "$WORK/graceful.sh"

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[G]
path = $WORK/graceful.sh
working_dir = $WORK
graceful_timeout = 5
max_restarts = 3
EOF

CHILD_PATTERN="$WORK/graceful.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "进程没有被启动"
sleep 1
stop_daemon 20 || fail "守护进程没有在超时内退出"

if [ ! -f "$WORK/term.flag" ]; then
    fail "子进程没有收到 SIGTERM（优雅关闭未生效）"
fi
if ! grep -q "Sending SIGTERM to PID" "$WORK/daemon.log"; then
    fail "日志里没有发送 SIGTERM 的记录"
fi
if grep -q "Force terminating" "$WORK/daemon.log"; then
    fail "子进程已经优雅退出，却仍然执行了 SIGKILL"
fi
pass "优雅关闭生效：子进程收到 SIGTERM 并自行退出"
