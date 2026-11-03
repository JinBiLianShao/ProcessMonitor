#!/usr/bin/env bash
# pid 文件必须互斥，第二个实例要拒绝启动。
source "$(dirname "$0")/lib.sh" "$@"
setup

write_child sleeper.sh 'exec /bin/sleep 30'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1
pid_file = $WORK/pm.pid

[One]
path = $WORK/sleeper.sh
working_dir = $WORK
EOF

CHILD_PATTERN="$WORK/sleeper.sh"
start_daemon

wait_for_log "Pid file acquired" 10 || fail "第一个实例没有获取 pid 文件"

if "$PM_BIN" -c "$WORK/config.ini" >"$WORK/second.log" 2>&1; then
    fail "第二个实例竟然启动成功了"
fi
if ! grep -q "Another instance already holds" "$WORK/second.log"; then
    fail "第二个实例的拒绝原因不是 pid 文件锁"
fi

stop_daemon 15 || fail "守护进程没有在超时内退出"
pass "pid 文件互斥生效"
