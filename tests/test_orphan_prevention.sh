#!/usr/bin/env bash
# 守护进程被 SIGKILL 时，子进程必须收到 SIGTERM（PR_SET_PDEATHSIG），不能变成孤儿。
source "$(dirname "$0")/lib.sh" "$@"
setup

cat >"$WORK/orphan.sh" <<EOF
#!/bin/sh
trap 'echo TERM > $WORK/orphan.flag; exit 0' TERM
while true; do sleep 0.1; done
EOF
chmod +x "$WORK/orphan.sh"

cat >"$WORK/config.ini" <<EOF
[Global]
log_level = INFO
log_file = $WORK/daemon-file.log
check_interval = 1

[Orphan]
path = $WORK/orphan.sh
working_dir = $WORK
graceful_timeout = 1
EOF

CHILD_PATTERN="$WORK/orphan.sh"
start_daemon

wait_for_log "Started Unix process" 10 || fail "进程没有被启动"
sleep 1

kill -9 "$DAEMON_PID" 2>/dev/null
wait "$DAEMON_PID" 2>/dev/null
DAEMON_PID=""

for i in $(seq 1 40); do
    if [ -f "$WORK/orphan.flag" ]; then
        break
    fi
    sleep 0.1
done

if [ ! -f "$WORK/orphan.flag" ]; then
    fail "守护进程被 SIGKILL 后子进程没有收到 SIGTERM（留下孤儿）"
fi
pass "子进程收到 PDEATHSIG，未留下孤儿"
