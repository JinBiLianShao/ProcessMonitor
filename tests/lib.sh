#!/usr/bin/env bash
# 测试公共函数。所有测试都在临时目录里跑，互不干扰。
set -o pipefail

PM_BIN=""
WORK=""
DAEMON_PID=""
CHILD_PATTERN=""

if [ $# -ge 1 ]; then
    PM_BIN="$1"
fi

fail() {
    echo "FAIL: $*" >&2
    if [ -n "$WORK" ] && [ -f "$WORK/daemon.log" ]; then
        echo "--- daemon.log ---" >&2
        cat "$WORK/daemon.log" >&2
    fi
    exit 1
}

pass() {
    echo "PASS: $*"
}

setup() {
    [ -n "$PM_BIN" ] || { echo "FAIL: no binary path given" >&2; exit 1; }
    [ -x "$PM_BIN" ] || { echo "FAIL: $PM_BIN is not executable" >&2; exit 1; }
    WORK="$(mktemp -d)"
    trap cleanup EXIT
}

cleanup() {
    if [ -n "$DAEMON_PID" ]; then
        kill -9 "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
    fi
    if [ -n "$CHILD_PATTERN" ]; then
        pkill -9 -f "$CHILD_PATTERN" 2>/dev/null
    fi
    if [ -n "$WORK" ]; then
        rm -rf "$WORK"
    fi
}

# 启动守护进程（后台），日志写到 $WORK/daemon.log
start_daemon() {
    "$PM_BIN" -c "$WORK/config.ini" >"$WORK/daemon.log" 2>&1 &
    DAEMON_PID=$!
}

# 等待日志里出现某个模式，最多等 $2 秒
wait_for_log() {
    local pattern="$1"
    local timeout=10
    local i
    if [ $# -ge 2 ]; then
        timeout="$2"
    fi
    for ((i = 0; i < timeout * 10; i++)); do
        if grep -qE "$pattern" "$WORK/daemon.log" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 让守护进程优雅退出，最多等 $1 秒
stop_daemon() {
    local timeout=15
    local i
    if [ $# -ge 1 ]; then
        timeout="$1"
    fi
    if [ -z "$DAEMON_PID" ]; then
        return 0
    fi
    kill -TERM "$DAEMON_PID" 2>/dev/null
    for ((i = 0; i < timeout * 10; i++)); do
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            wait "$DAEMON_PID" 2>/dev/null
            DAEMON_PID=""
            return 0
        fi
        sleep 0.1
    done
    kill -9 "$DAEMON_PID" 2>/dev/null
    wait "$DAEMON_PID" 2>/dev/null
    DAEMON_PID=""
    return 1
}

write_child() {
    local name="$1"
    local body="$2"
    printf '#!/bin/sh\n%s\n' "$body" >"$WORK/$name"
    chmod +x "$WORK/$name"
}
