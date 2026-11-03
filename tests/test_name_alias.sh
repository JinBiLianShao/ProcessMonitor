#!/usr/bin/env bash
# 显示名（name 键）与配置节名不同时，停止流程必须仍能找到该进程。
# 旧实现的映射以显示名为键、查找以节名为键，两者不一致时
# stopProcess 直接返回 false，子进程根本收不到 SIGTERM。
source "$(dirname "$0")/lib.sh" "$@"
setup

cat >"$WORK/alias.sh" <<EOF
#!/bin/sh
trap 'echo TERM > $WORK/alias.flag; exit 0' TERM
while true; do sleep 0.1; done
EOF
chmod +x "$WORK/alias.sh"

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[SectionName]
name = DisplayName
path = $WORK/alias.sh
working_dir = $WORK
graceful_timeout = 5
max_restarts = 3
EOF

CHILD_PATTERN="$WORK/alias.sh"
start_daemon

wait_for_log "Started Unix process: DisplayName" 10 || fail "进程没有被启动"
sleep 1
stop_daemon 20 || fail "守护进程没有在超时内退出"

if [ ! -f "$WORK/alias.flag" ]; then
    fail "name 与节名不一致时子进程没有收到 SIGTERM"
fi
if ! grep -q "Sending SIGTERM to PID" "$WORK/daemon.log"; then
    fail "日志里没有发送 SIGTERM 的记录"
fi
pass "显示名与节名不一致时仍能正确定位并优雅停止"
