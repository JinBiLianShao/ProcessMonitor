#!/usr/bin/env bash
# 缺键时必须使用默认值，而不是读取未初始化内存。
# 旧版：配置里漏写 max_restarts 会让第一轮检查就判定"重启次数超限"。
source "$(dirname "$0")/lib.sh" "$@"
setup

write_child sleeper.sh 'exec /bin/sleep 30'

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = DEBUG
log_file = $WORK/daemon-file.log
check_interval = 1

[Partial]
path = $WORK/sleeper.sh
working_dir = $WORK
max_restarst = 999
EOF

CHILD_PATTERN="$WORK/sleeper.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "进程没有被启动（缺键时默认值失效）"

if grep -q "Max restarts reached" "$WORK/daemon.log"; then
    fail "缺键导致立刻判定重启超限"
fi
if ! grep -q "Unknown key in \[Partial\]: max_restarst" "$WORK/daemon.log"; then
    fail "拼错的键没有产生警告"
fi

stop_daemon 15 || fail "守护进程没有在超时内退出"
pass "缺键使用默认值，未知键产生警告"
