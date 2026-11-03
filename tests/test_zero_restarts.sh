#!/usr/bin/env bash
# max_restarts = 0 的语义是"只启动，不重启"。
# 不能因为预算为 0 就一次都不启动 —— 这是把首次启动误算成一次重启。
source "$(dirname "$0")/lib.sh" "$@"
setup

write_child once.sh 'exit 0'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Once]
path = $WORK/once.sh
working_dir = $WORK
restart_delay = 0
max_restarts = 0
graceful_timeout = 1
EOF

CHILD_PATTERN="$WORK/once.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "max_restarts=0 时首次启动被错误地阻止"
sleep 3
stop_daemon 20 || true

starts="$(grep -c "Started Unix process" "$WORK/daemon.log")"
if [ "$starts" -ne 1 ]; then
    fail "max_restarts=0 应只启动 1 次，实际 ${starts} 次"
fi
if ! grep -q "Max restarts reached" "$WORK/daemon.log"; then
    fail "首次启动后再退出，应当判定预算耗尽"
fi
pass "max_restarts=0 启动一次且不重启"
