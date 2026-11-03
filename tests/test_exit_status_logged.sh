#!/usr/bin/env bash
# 子进程的退出码必须落日志。旧版 waitpid 拿到 status 后直接丢弃。
source "$(dirname "$0")/lib.sh" "$@"
setup

write_child exiter.sh 'exit 7'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Exiter]
path = $WORK/exiter.sh
working_dir = $WORK
restart_delay = 0
max_restarts = 1
graceful_timeout = 1
EOF

CHILD_PATTERN="$WORK/exiter.sh"
start_daemon

wait_for_log "exited with code 7" 20 || fail "退出码没有落日志"
stop_daemon 15 || true
pass "退出码已落日志"
