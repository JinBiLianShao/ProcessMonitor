#!/usr/bin/env bash
# exec 失败必须判为启动失败：不能产生假 PID，也不能假装成功。
source "$(dirname "$0")/lib.sh" "$@"
setup

# shebang 指向不存在的解释器：文件存在且可执行，但 execve 会失败
printf '#!/nonexistent/interpreter\nexit 0\n' >"$WORK/broken.sh"
chmod +x "$WORK/broken.sh"

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Broken]
path = $WORK/broken.sh
working_dir = $WORK
restart_delay = 0
max_restarts = 2
graceful_timeout = 1
EOF

start_daemon

wait_for_log "Failed to exec" 20 || fail "exec 失败没有被识别"
stop_daemon 15 || true

if grep -q "Started Unix process: Broken" "$WORK/daemon.log"; then
    fail "exec 失败却记录了'启动成功'"
fi
if ! grep -q "No such file or directory" "$WORK/daemon.log"; then
    fail "没有把子进程的 errno 原因回传到日志"
fi
pass "exec 失败被判为启动失败，errno 已回传"
