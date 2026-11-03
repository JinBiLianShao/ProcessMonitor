# ProcessMonitor

一个 Linux 下的小型进程守护程序（daemon）。你给它一份配置文件，它负责：

- 把配置里写的那几个程序**拉起来**
- 程序**挂了自动重启**（带退避，不会疯狂重试）
- 你要它停的时候**先礼后兵**（先发 SIGTERM 让它自己收拾，超时才强杀）
- 自己挂掉时**不留下孤儿进程**

代码不到 2000 行，没有第三方依赖，`g++` 就能编。

---

## 目录

- [一、5 分钟跑起来](#一5-分钟跑起来)
- [二、配置文件逐项详解](#二配置文件逐项详解)
- [三、日志怎么看](#三日志怎么看)
- [四、常见场景怎么配](#四常见场景怎么配)
- [五、它到底是怎么工作的](#五它到底是怎么工作的)
- [六、编译与构建](#六编译与构建)
- [七、测试](#七测试)
- [八、排查问题](#八排查问题)
- [九、这一版修了什么](#九这一版修了什么)
- [十、已知限制](#十已知限制)

---

## 一、5 分钟跑起来

### 1. 编译

    cd ProcessMonitor
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

编译完会得到一个可执行文件：`build/ProcessMonitor`。

> 不想用 cmake 也行，直接：
>
>     g++ -std=c++17 -O2 -pthread -o ProcessMonitor main.cpp process.cpp
>
> 只是这样就跑不了测试。

### 2. 写一份最简单的配置

新建 `my.ini`，内容：

    [Global]
    log_level = INFO
    log_file = /tmp/pm.log
    check_interval = 2

    [MyApp]
    path = /bin/sleep
    args = 3600

这配置的意思是：启动 `/bin/sleep 3600` 并一直盯着它，每 2 秒看一次，日志写到 `/tmp/pm.log`。

### 3. 跑起来

    ./build/ProcessMonitor -c my.ini

你会看到：

    [2026-11-03 10:27:20.073] [INFO] Process Daemon Initialized
    [2026-11-03 10:27:20.073] [INFO] Loaded 1 process configurations
    [2026-11-03 10:27:20.073] [INFO] Started monitor for: MyApp
    Process daemon is running. Send SIGINT/SIGTERM to stop.
    [2026-11-03 10:27:20.073] [WARNING] Process MyApp is not running. Restarting... (1/5)
    [2026-11-03 10:27:20.074] [INFO] Started Unix process: MyApp PID: 39543

那一行 `WARNING ... Restarting... (1/5)` 看着像出错了，其实不是 —— 首次启动也走这条路径，因为程序不知道"从没启动过"和"启动过但挂了"的区别。

### 4. 验证它真的在守护

开另一个终端，先找到子进程的 pid，再杀掉它：

    pgrep -P $(pgrep -f 'ProcessMonitor -c my.ini')     # 子进程 pid
    kill -9 <上面查到的 pid>

或者简单点，直接按名字杀（这台机器上只有一个 sleep 的话）：

    pkill -9 -x sleep

> 别用 `pgrep -f "sleep 3600"` —— `-f` 会匹配完整命令行，很可能把你自己的 shell 也匹配进去，结果杀错东西。

然后看日志（下面是实测输出）：

    [WARNING] Process MyApp (PID 54102) killed by signal 9 (Killed) after 2s
    [INFO] Waiting 3s before restarting MyApp
    [WARNING] Process MyApp is not running. Restarting... (2/5)
    [INFO] Started Unix process: MyApp PID: 54113

注意三点：

- **退出原因**：`killed by signal 9 (Killed)` —— 是被 `kill -9` 干掉的
- **活了多久**：`after 2s` —— 从启动到退出经过的时间。这个数字很关键：跑 0 秒就死通常是配置问题，跑几小时才死通常是内存泄漏
- **等了 3 秒才重启**：这就是 `restart_delay` 在起作用（日志里的 `Waiting 3s` 那行）

时间戳对一下能看出退避是真实的：

    11:01:08  子进程被杀
    11:01:10  开始等 3 秒
    11:01:13  真正重启（10 + 3 = 13）

### 5. 停掉

在跑 daemon 的那个终端按 `Ctrl+C`。

如果在别的终端，就给它发 SIGTERM：

    kill -TERM $(pgrep -f 'ProcessMonitor -c my.ini')

> 前面那份 `my.ini` 没配 `pid_file`，所以只能靠 pgrep 找。如果你在 `[Global]` 里加了 `pid_file = /tmp/pm.pid`，就可以 `kill -TERM $(cat /tmp/pm.pid)`，更可靠。

你会看到（实测输出）：

    Received signal 15 (Terminated), shutting down...
    [INFO] Stopping all processes...
    [INFO] Monitor thread stopped for: MyApp
    [INFO] Sending SIGTERM to PID 54113 (graceful timeout 10s)
    [INFO] Process MyApp (PID 54113) killed by signal 15 (Terminated) after 1s (stopping)
    [INFO] Process 54113 terminated gracefully
    [INFO] All processes stopped
    Process daemon stopped.

这里有两个 `PID 54113` 的行，别以为是重复：

- `killed by signal 15 ... (stopping)` 是**收尸线程**报的，说明子进程确实退出了，退因是 SIGTERM
- `terminated gracefully` 是**停止流程**报的，说明它在我们等待的时限内自己退出了，不需要强杀

末尾的 `(stopping)` 很关键：有这个词说明是**我们自己让它停的**（正常），没有就是意外崩溃（会触发重启）。

整个停机过程 200ms 左右 —— `/bin/sleep` 收到 SIGTERM 立刻退出，没走到强杀那条路。

---

## 二、配置文件逐项详解

配置文件是 INI 格式。三个规则：

1. `[方括号]` 开头的是**节**（section）
2. `键 = 值` 是配置项，等号两边空格会被自动去掉
3. `#` 或 `;` 开头的整行是注释

**没有行尾注释**。`log_level = INFO  # 日志级别` 里的 `# 日志级别` 会被当成值的一部分，然后解析失败打警告。

配置文件分两类节：

- `[Global]` —— 全局设置，只有这一个
- **其他任何节** —— 都当作一个要托管的进程，节名就是它的内部标识

### 2.1 [Global] 全局配置

| 配置项 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `log_level` | 字符串 | `INFO` | 日志级别 |
| `log_file` | 路径 | 空 | 日志文件。留空则只输出到控制台 |
| `check_interval` | 整数（秒） | `2` | 多久检查一次进程状态 |
| `pid_file` | 路径 | 空 | pid 文件。留空则不启用单实例锁 |

#### log_level

四个值，从啰嗦到安静：

| 值 | 什么时候用 |
|----|-----------|
| `DEBUG` | 排查问题时。会把"进程已回收"这类内部动作也打出来，日志量很大 |
| `INFO` | **默认，日常用这个**。启动、停止、重启、退出码都在这一级 |
| `WARNING` | 只想看异常：进程挂了、配置项写歪了 |
| `ERROR` | 只想看"起不来"和"彻底放弃" |

注意这是**阈值**不是过滤器：设成 `WARNING` 就看不到 `INFO` 的启动记录了。

#### log_file

日志会**同时**输出到控制台和这个文件。写相对路径时是相对于**你执行命令时所在的目录**，不是配置文件所在目录 —— 所以建议写绝对路径。

目录必须已经存在，程序不会帮你建（这是故意的，免得因为路径打错在莫名其妙的地方建一堆目录）。

#### check_interval

守护进程每隔多少秒检查一次"我管的进程还活着吗"。

- 允许范围 **1 ~ 3600**，超出会被钳到边界并打警告
- **不能填 0**。填 0 会被钳成 1 —— 老版本填 0 会让等待循环跑 0 次，整个循环变成纯忙等，CPU 直接 99%
- 填太小（1 秒）意味着进程退出后最多 1 秒就被发现，代价是每秒醒一次
- 填太大（60 秒）意味着进程挂了要最多 60 秒才被拉起

**这个值不影响重启延迟**，那是 `restart_delay` 管的事。

#### pid_file

写一个路径，程序启动时会独占锁住这个文件，并把自己的 pid 写进去。

- 作用是**防止启动两个实例**。两个实例抢着拉同一批进程，结果是每个进程被拉两份，然后互相抢端口，现场很难看
- 第二个实例会直接退出并打 `Another instance already holds ...`
- 程序正常退出时会删掉这个文件
- 用了 `flock`，所以进程被 `kill -9` 之后锁也会自动释放，不会出现"僵尸锁"

建议加上，比如 `pid_file = /var/run/processmonitor.pid` 或 `/tmp/pm.pid`。

### 2.2 进程配置节

每个非 `[Global]` 的节代表一个要托管的进程。下面用这个例子讲：

    [MyWebApp]
    name = 我的网站
    path = /opt/app/server
    working_dir = /opt/app
    args = --port 8080 --workers 4
    env_vars = APP_ENV=prod,LOG_DIR=/var/log/app
    restart_delay = 3
    restart_backoff_max = 60
    max_restarts = 5
    graceful_timeout = 10
    restart_reset_after = 60

#### name —— 显示名

| | |
|---|---|
| 类型 | 字符串 |
| 默认值 | 配置节名（例子里是 `MyWebApp`） |

**只影响日志里的显示**。程序内部认的是节名，所以改 `name` 不影响任何行为。

但要注意：日志里出现的就是这个名字，所以名字重复会让日志分不清是谁 —— 程序会打一条 `ERROR ... logs will be ambiguous` 提醒你，但不会阻止启动。

> 提醒：以前有个 bug 是"存 pid 用显示名、查 pid 用节名"，一旦你写了 `name`，停止流程就失效了（子进程收不到 SIGTERM）。现在已修，有测试盯着（`test_name_alias`）。

#### path —— 可执行文件

| | |
|---|---|
| 类型 | 路径 |
| 默认值 | 无，**必填** |

要启动的程序。可以是绝对路径，也可以是相对路径（相对 `working_dir` 解析）。

几个要点：

- **必须是普通文件**，不能是目录。程序会检查，不通过就把这个节整个丢掉
- 必须存在。启动时不存在的话，配置校验阶段就会被剔除，日志里会打 `Executable does not exist`
- **必须可执行**（有 x 权限），否则 `exec` 会失败，日志打 `Failed to exec ...: Permission denied`
- **不经过 shell**。所以 `path = /bin/bash -c "xxx"` 这种写法是错的 —— 参数要放 `args` 里

#### working_dir —— 工作目录

| | |
|---|---|
| 类型 | 路径 |
| 默认值 | `path` 所在的目录 |

子进程启动时会 `chdir` 到这个目录。影响：

- 程序里所有相对路径的解析基准
- 程序写相对路径文件（日志、缓存）的位置
- `path` 和 `args` 里的相对路径也以它为基准

留空的话，默认用 `path` 的父目录，这通常是你想要的。

目录不存在时程序会尝试创建（**只建一级**，不递归）。建不出来只打警告不阻止启动 —— 也许你挂的是只读目录，程序自己会处理。

#### args —— 命令行参数

| | |
|---|---|
| 类型 | 字符串 |
| 默认值 | 空 |

传给程序的参数，空格分隔。支持单引号和双引号：

    args = --port 8080
    args = --name "hello world" --verbose
    args = --pattern '*.log'

引号会被吃掉，不传给程序 —— 上面第二条会拆成三个参数：`--name`、`hello world`、`--verbose`。

**这不是 shell**，所以：

- 没有变量展开：`args = --home $HOME` 会原样传 `$HOME` 五个字符
- 没有管道重定向：`args = | tee out.txt` 是错的
- 没有通配符：`args = *.log` 不会展开

要这些能力就自己写个 `.sh` 包一层，然后把 `path` 指向脚本。这是故意的设计 —— 经过 shell 就有命令注入风险。

#### env_vars —— 环境变量

| | |
|---|---|
| 类型 | 逗号分隔的 `KEY=VALUE` 列表 |
| 默认值 | 空（完全继承守护进程的环境） |

**语义是"继承 + 覆盖"**，不是"只用这些"。也就是说子进程会拿到守护进程的全部环境变量（`PATH`、`HOME` 等），你写的这些只是额外覆盖或追加。

    env_vars = APP_ENV=prod,LOG_DIR=/var/log/app

支持引用当前环境变量：

    env_vars = LOG_DIR=${HOME}/logs,APP_ENV=prod

`${HOME}` 会在**守护进程启动时**展开成实际值，找不到的变量会展开成空串并打警告。

三个坑：

1. **值里不能带逗号**。因为逗号是分隔符。`env_vars = MSG=a,b` 会被拆成 `MSG=a` 和 `b`（后者因为没等号被丢弃并警告）
2. 键值前后可以有空格，会被自动去掉：`A=1, B=2` 里的 ` B` 没问题
3. **只对子进程生效**。守护进程自己的环境不会变

#### restart_delay —— 首次重启延迟

| | |
|---|---|
| 类型 | 整数（秒） |
| 默认值 | `3` |
| 范围 | 0 ~ 86400 |

进程挂了之后**等多久才重启**。填 0 就是立刻重启（不推荐，会疯狂刷日志）。

但注意这是**首次**延迟。实际延迟会指数增长，见下一个参数。

> 老版本这个参数解析了但**从来没被用过** —— 配置里写"等 10 秒"，实际每 `check_interval` 就拉一次。现在是真的生效了。

#### restart_backoff_max —— 退避上限

| | |
|---|---|
| 类型 | 整数（秒） |
| 默认值 | `60` |
| 范围 | 0 ~ 86400 |

实际的重启延迟是这么算的：

    第 1 次重启：restart_delay × 1
    第 2 次重启：restart_delay × 2
    第 3 次重启：restart_delay × 4
    第 4 次重启：restart_delay × 8
    ...
    封顶：restart_backoff_max

举例，`restart_delay = 3`、`restart_backoff_max = 60`：

    3 → 6 → 12 → 24 → 48 → 60 → 60 → 60 ...

**为什么要退避？** 如果程序启动就崩（比如配置文件写错了），不退避的话你会看到：

    10:00:00 Started
    10:00:00 exited with code 1
    10:00:00 Started
    10:00:00 exited with code 1
    ...（每秒几十次，日志刷爆，磁盘写满）

退避之后崩溃循环变得温和，日志还有救，而且能一眼看出"这是个持续性故障"。

#### max_restarts —— 最多重启几次

| | |
|---|---|
| 类型 | 整数 |
| 默认值 | `5` |
| 范围 | -1 ~ 1000000 |

**这是"重启"次数，不含首次启动。**

| 值 | 含义 |
|----|------|
| `5` | 首次启动 + 最多重启 5 次，之后放弃 |
| `0` | **只启动一次，挂了就不管了** |
| `-1` | 无限重启（永不放弃） |

预算烧完之后：

- 打 `ERROR Max restarts reached for: xxx. Giving up; the process is left in FAILED state.`
- 监控线程**退出**
- **守护进程本身继续运行**，其他进程照常被守护

⚠️ 这是最容易被忽略的状态：某个进程彻底放弃之后，从外部看"守护进程还在跑"，但它已经不管那个进程了。如果你要"永不放弃"，就写 `-1`。

#### graceful_timeout —— 优雅关闭等待时间

| | |
|---|---|
| 类型 | 整数（秒） |
| 默认值 | `10` |
| 范围 | 0 ~ 3600 |

停止一个进程时的流程：

    1. 给整个进程组发 SIGTERM
    2. 每秒检查 10 次，等它自己退出
    3. 等满 graceful_timeout 还没退 → 发 SIGKILL 强杀
    4. 再等 5 秒确认死透

所以填多少，决定了"停机时最多能等多久"。

| 场景 | 建议值 |
|------|--------|
| 无状态的小工具 | `2` ~ `5` |
| Web 服务（要处理完手头请求） | `10` ~ `30` |
| 数据库 / 有事务的 | `30` ~ `60` |

填 `0` 表示**不等待，直接 SIGKILL**。除非你确定进程没有需要清理的状态，否则别填 0。

> 老版本这里永远是 0（代码里 `stopAll` 直接传了 `force=true`），`graceful_timeout` 完全是摆设，子进程连收拾后事的机会都没有。现在真的会先礼后兵了。

#### restart_reset_after —— 重启预算重置

| | |
|---|---|
| 类型 | 整数（秒） |
| 默认值 | `60` |
| 范围 | 0 ~ 86400 |

**这个参数解决的是一个很阴的问题。**

`max_restarts` 的计数是累计的。假设你的服务配了 `max_restarts = 5`，它稳定跑了三个月，期间因为偶发原因崩了 5 次（每次都被成功重启）。第六次崩溃时，预算已经用完，守护进程直接放弃 —— 一个健康运行了三个月的服务，因为累计五次历史故障就永久躺下了。

`restart_reset_after` 的意思是：**进程这次运行如果活过了这么多秒，就把重启计数清零**。

    进程存活 >= restart_reset_after  →  重启计数归零
    进程存活 <  restart_reset_after  →  计数继续累加

这样：

- "启动就崩" → 每次都不够久 → 计数快速烧完 → 及时放弃（正确，说明是持续性故障）
- "跑很久才偶尔崩" → 每次都够久 → 计数总是清零 → 永远不会放弃（正确，说明是健康的）

填 `0` 表示**不重置**，回到老行为。

---

## 三、日志怎么看

### 格式

    [2026-11-03 10:27:20.073] [LEVEL] 消息内容

时间精确到毫秒。**多线程写日志时毫秒很有用**，能看出事件的实际先后。

### 各级别的典型消息

**启动阶段**

    [INFO] Process Daemon Initialized
    [INFO] Loaded 3 process configurations
    [INFO] Pid file acquired: /tmp/pm.pid
    [INFO] Started monitor for: web
    [INFO] Started Unix process: web PID: 12345

**正常退出（你让它停的）**

    [INFO] Process web (PID 12345) exited with code 0 after 3600s (stopping)

注意末尾的 `(stopping)` —— 有这个词说明是正常停机流程，不是故障。

**异常退出（要重启的）**

    [WARNING] Process web (PID 12345) killed by signal 11 (Segmentation fault) after 120s
    [INFO] Waiting 6s before restarting web
    [WARNING] Process web is not running. Restarting... (2/5)
    [INFO] Started Unix process: web PID: 12346

三个信息量很大的地方：

- `killed by signal 11` → 段错误，程序有 bug
- `after 120s` → 跑了 2 分钟才崩，可能是内存泄漏
- `Restarting... (2/5)` → 已经用掉 2 次预算

**进程彻底放弃**

    [ERROR] Max restarts reached for: web. Giving up; the process is left in FAILED state.

看到这条要人工介入了。守护进程不会再管这个进程。

**exec 失败**

    [ERROR] Failed to exec /opt/app/server for web: Permission denied (errno: 13)

**关键**：这时日志里**不会**出现 `Started Unix process`，因为确实没启动成功，重启预算也不消耗。老版本会假装成功并报一个不存在的 PID。

**配置有问题**

    [WARNING] Unknown key in [web]: max_restart                    ← 拼错了
    [WARNING] Value out of range for check_interval in [Global]: 0 (allowed 1..3600), clamping
    [WARNING] Environment variable HOME_DIR not defined
    [ERROR]   Executable does not exist: /opt/app/serve            ← 路径错了

**停机流程**

    Received signal 15 (Terminated), shutting down...
    [INFO] Stopping all processes...
    [INFO] Monitor thread stopped for: web
    [INFO] Sending SIGTERM to PID 12346 (graceful timeout 10s)
    [INFO] Process 12346 terminated gracefully        ← 顺利退出
    [INFO] All processes stopped

如果看到这个：

    [WARNING] Process 12346 did not exit within 10s
    [WARNING] Force terminating process group: 12346
    [INFO] Process web (PID 12346) killed by signal 9 (Killed) after 4000s (stopping)

说明进程**不响应 SIGTERM**。要么是它自己忽略了，要么 `graceful_timeout` 给太短了。

### 快速过滤常用命令

    # 只看异常
    grep -E 'WARNING|ERROR' /tmp/pm.log

    # 看某个进程的全部记录
    grep 'web' /tmp/pm.log

    # 统计重启次数
    grep -c 'Started Unix process: web' /tmp/pm.log

    # 看谁彻底放弃了
    grep 'Max restarts reached' /tmp/pm.log

    # 看退出原因分布
    grep -oE 'exited with code [0-9]+|killed by signal [0-9]+' /tmp/pm.log | sort | uniq -c

---

## 四、常见场景怎么配

### 场景 1：守护一个 Web 服务

    [Global]
    log_level = INFO
    log_file = /var/log/processmonitor.log
    check_interval = 2
    pid_file = /var/run/processmonitor.pid

    [WebApp]
    name = web-app
    path = /opt/webapp/bin/server
    working_dir = /opt/webapp
    args = --config /opt/webapp/conf/prod.yml
    env_vars = APP_ENV=production,LOG_DIR=/var/log/webapp
    restart_delay = 2
    restart_backoff_max = 30
    max_restarts = -1          # 永不放弃，交给人工监控
    graceful_timeout = 30      # 给足时间处理完手头请求
    restart_reset_after = 120  # 稳定跑 2 分钟就清空预算

### 场景 2：跑一个会自己正常退出的批处理

有些程序是"干完活就退出"的，不该重启：

    [BatchJob]
    path = /opt/jobs/nightly.sh
    working_dir = /opt/jobs
    max_restarts = 0           # 只启动一次，退出就退出
    graceful_timeout = 5

如果想让它在退出后**再拉起来**（比如做成循环）：

    [BatchJob]
    path = /opt/jobs/nightly.sh
    max_restarts = -1
    restart_delay = 60         # 隔一分钟再来一轮
    restart_backoff_max = 60   # 固定间隔，不翻倍

### 场景 3：同一个程序跑多个实例

用不同的节名，配不同的参数：

    [Worker1]
    path = /opt/app/worker
    args = --queue high
    working_dir = /opt/app

    [Worker2]
    path = /opt/app/worker
    args = --queue low
    working_dir = /opt/app

两个实例各自独立重启、独立计数。

### 场景 4：跑一个 Python 脚本

`path` 必须是可执行文件，所以要么给脚本加 shebang + x 权限：

    #!/usr/bin/env python3
    # script.py

    [PyScript]
    path = /opt/scripts/worker.py
    working_dir = /opt/scripts

    chmod +x /opt/scripts/worker.py

要么直接用解释器：

    [PyScript]
    path = /usr/bin/python3
    args = /opt/scripts/worker.py --verbose
    working_dir = /opt/scripts

**推荐第二种**，因为路径更明确，不依赖 shebang 和 PATH。

### 场景 5：需要环境变量才能跑

    [MyApp]
    path = /opt/app/server
    env_vars = DATABASE_URL=postgres://localhost/mydb,DEBUG=0,CONFIG_PATH=${HOME}/.myapp

注意逗号是分隔符，所以 `DATABASE_URL` 的值里不能有逗号。

### 场景 6：崩溃循环时想快速止损

    [UnstableApp]
    path = /opt/app/unstable
    restart_delay = 5
    restart_backoff_max = 300    # 最多等 5 分钟
    max_restarts = 3             # 崩 3 次就别试了
    restart_reset_after = 0      # 不重置预算，累计 3 次就放弃

这样崩溃循环会在几分钟内自动停下来，不会拖垮机器。

---

## 五、它到底是怎么工作的

### 线程模型

    主线程
      └─ 解析命令行、读配置、sigtimedwait 等信号

    监控线程 × N（每个进程一个）
      └─ 循环：查状态 → 死了就重启 → 睡 check_interval

    收尸线程 × 1（全进程只有一个）
      └─ 循环：waitpid(-1, WNOHANG) 回收所有退出的子进程

    └─ 子进程 × N（你配置的那些程序）

**为什么收尸线程只有一个？** 因为一个子进程只能被 `waitpid` 一次。多个线程同时 `waitpid(-1)` 会互相抢，谁抢到算谁的，结果就是某个进程的退出码丢了、重启计数乱了。

这个线程同时维护着"谁还活着"的**唯一真相**：pid 从表里删掉就代表进程真的没了。别的代码要判活就查这张表 —— **不能用 `kill(pid, 0)`**，因为僵尸进程对它的返回值也是 0。

### 一个进程的完整生命周期

    ① 读配置，校验（path 存在吗？数值合法吗？）
            ↓ 不合法 → 从配置里剔除，打 ERROR
    ② 启动：fork → setsid → chdir → execve
            ↓ execve 失败 → 通过管道把 errno 回传，判为启动失败
    ③ 运行中：监控线程每 check_interval 秒查一次
            ↓ 还活着 → 继续睡
    ④ 进程退出：收尸线程 waitpid 拿到它，记录退出码/信号/存活时长
            ↓ 是我们让它停的 → 打 INFO，结束
            ↓ 是意外退出 → 打 WARNING，走重启流程
    ⑤ 重启：检查预算 → 算退避延迟 → 等待 → 回到 ②
            ↓ 预算耗尽 → 打 ERROR，放弃，线程退出
    ⑥ 停机：SIGTERM → 等 graceful_timeout → SIGKILL
            ↓ 收尸线程确认全部回收 → 进程退出

### 停止时的顺序（很重要）

    global_stop_ = true          ← 先拉总闸
    join 所有监控线程              ← 确保不会再拉新进程
    逐个优雅停止（SIGTERM）        ← 这时候才动子进程
    join 收尸线程                  ← 收干净
    释放 pid 文件

顺序不能乱。如果先停子进程再拉闸，监控线程会发现"进程没了"然后立刻又拉起来 —— 你停一个它拉一个，永远停不完。

### 信号处理

程序**不在信号处理函数里做任何工作**。

信号处理函数可能打断任何线程，包括正拿着日志锁的那个。这时候再去 `join` 线程或抢锁就是自己锁自己，进程直接卡死。

所以做法是：

1. 创建任何线程**之前**，先把 `SIGINT/SIGTERM/SIGQUIT` 屏蔽掉
2. 所有线程都继承这个屏蔽字，信号不会跑到某个线程里去执行 handler
3. 主线程在循环里用 `sigtimedwait` 同步地把信号取出来
4. 取到之后正常调用 `stopAll()` —— 这时候没有并发问题

**注意一个连带影响**：这个屏蔽字会被 `fork` 出来的子进程继承。子进程如果在 `exec` 之前不清掉，就永远收不到 `SIGTERM` —— 优雅关闭直接失效。代码里在 `createUnixProcess` 里做了这件事，改的时候别漏。

### 孤儿进程保护

守护进程被 `kill -9` 时，子进程不会跟着死 —— 它们是独立会话（`setsid` 过的）。

所以子进程在 `exec` 之前会调：

    prctl(PR_SET_PDEATHSIG, SIGTERM)

这是 Linux 特有的：内核会盯着父进程，父进程一死就给子进程发 `SIGTERM`。

---

## 六、编译与构建

### 基本构建

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

产物：`build/ProcessMonitor`

### 构建选项

| 选项 | 取值 | 默认 | 说明 |
|------|------|------|------|
| `CMAKE_BUILD_TYPE` | `Release` / `Debug` | `Release` | 构建类型 |
| `COMPILER_TARGET` | `x86` / `arm32` / `arm64` | `x86` | 目标架构 |
| `TARGET_TYPE` | `executable` / `static` / `shared` | `executable` | 产物类型 |
| `ENABLE_SANITIZER` | `OFF` / `address` / `thread` / `undefined` | `OFF` | 消毒器 |
| `WARNINGS_AS_ERRORS` | `ON` / `OFF` | `ON` | 警告当错误 |
| `ENABLE_TESTS` | `ON` / `OFF` | `ON` | 是否构建测试 |

### 交叉编译

    # ARM64
    cmake -S . -B build-arm64 -DCOMPILER_TARGET=arm64 -DENABLE_TESTS=OFF
    cmake --build build-arm64 -j
    file build-arm64/ProcessMonitor
    # 应该看到: ELF 64-bit LSB pie executable, ARM aarch64

    # ARM32（需要 arm-linux-gnueabihf-g++ 在 PATH 里）
    cmake -S . -B build-arm32 -DCOMPILER_TARGET=arm32 -DENABLE_TESTS=OFF
    cmake --build build-arm32 -j

> 老版本的交叉编译是**完全失效的**：`-DCOMPILER_TARGET=arm64` 编译出来的还是 x86 二进制。原因是 `set(CMAKE_CXX_COMPILER ...)` 写在了 `project()` 之后，CMake 直接忽略。现在移到前面了。

### 用消毒器排查问题

    # 内存错误
    cmake -S . -B build-asan -DENABLE_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-asan -j

    # 数据竞争
    cmake -S . -B build-tsan -DENABLE_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-tsan -j

这项目是多线程的，改了进程控制相关的代码后建议至少跑一遍 TSan。

### 安装

    cmake --install build --prefix /usr/local

会装到：

- `/usr/local/bin/ProcessMonitor`
- `/usr/local/etc/processmonitor/config.ini`（配置模板）

---

## 七、测试

    cmake --build build -j
    ctest --test-dir build --output-on-failure

12 条测试，都是**真实行为验证**（真的起进程、真的发信号、真的量时间），不是 mock。

| 测试 | 验证什么 |
|------|---------|
| `test_config_defaults` | 配置缺键时用默认值，不读未初始化内存；拼错的键产生警告 |
| `test_check_interval_clamp` | `check_interval = 0` 被钳到 1，CPU 不会打满 |
| `test_restart_backoff` | `restart_delay` 真的生效，两次重启间隔不小于配置值 |
| `test_restart_budget_reset` | 健康运行够久后重启预算被清零 |
| `test_zero_restarts` | `max_restarts = 0` 会启动一次且不重启 |
| `test_graceful_shutdown` | 停止时子进程真的收到 SIGTERM 并能自行退出 |
| `test_name_alias` | `name` 与节名不同时，停止流程仍能正确定位进程 |
| `test_exec_failure` | exec 失败被判为失败，errno 回传，不产生假 PID |
| `test_exit_status_logged` | 退出码落日志 |
| `test_no_zombie` | 频繁重启时不积累僵尸进程 |
| `test_pidfile_lock` | 第二个实例被 pid 文件锁拒绝 |
| `test_orphan_prevention` | 守护进程被 SIGKILL 后子进程收到 PDEATHSIG |

跑单条：

    ctest --test-dir build -R graceful --output-on-failure

`ctest` 会自动打印失败测试的完整 daemon 日志，方便定位。

---

## 八、排查问题

### 启动就报 Failed to load config file

配置文件路径不对。默认读当前目录下的 `config.ini`，用 `-c` 指定别的路径。

### 报 Executable does not exist

`path` 指向的文件不存在。注意 `working_dir` 会影响相对路径的解析。

### 报 Failed to exec ...: Permission denied

文件存在但没有 x 权限：

    chmod +x /opt/app/server

### 进程起不来，日志里也没有 Started Unix process

说明 `exec` 失败了。往上找 `Failed to exec` 那一行，errno 会告诉你原因：

| errno | 原因 |
|-------|------|
| 2 `No such file or directory` | 路径不存在，或 shebang 指向的解释器不存在 |
| 13 `Permission denied` | 没有执行权限 |
| 8 `Exec format error` | 文件格式不对（比如 x86 二进制跑在 ARM 上） |

### 日志里有 Started Unix process 但进程立刻没了

看紧跟的那条 `exited with code N` 或 `killed by signal N`。

- `exited with code 1` → 程序自己的错误，去看它自己的日志
- `killed by signal 11` → 段错误
- `killed by signal 9` → 被 OOM killer 干掉了，`dmesg | tail` 能看到

### 进程一直在重启，日志刷屏

`restart_delay` 配太小或者 `max_restarts = -1`。先看退出原因 —— 如果是持续崩溃，退避策略会自然放缓；如果还嫌吵，把 `max_restarts` 调小让它早点放弃。

### 停机很慢，或者要等很久

看日志里有没有 `did not exit within Ns`：

- 有 → 进程不响应 SIGTERM。要么程序自己忽略了，要么 `graceful_timeout` 给太短
- 没有但停机慢 → 检查是不是 `check_interval` 太大

### 报 Another instance already holds

已经有一个实例在跑了。检查：

    cat /tmp/pm.pid
    ps -p $(cat /tmp/pm.pid)

如果那个进程已经不存在了，说明是残留文件（不该发生，`flock` 会自动释放）。直接删掉 pid 文件再启动。

### 进程死了但守护进程没重启它

三种可能：

1. **预算耗尽**：`grep 'Max restarts reached' 日志`
2. **`max_restarts = 0`**：配置就是"只启动不重启"
3. **守护进程自己也挂了**：`ps aux | grep ProcessMonitor`

### 想看程序实际收到了什么信号

用 `strace` 跟一下子进程：

    strace -f -e trace=signal -p $(cat /tmp/pm.pid)

---

## 九、这一版修了什么

上一版有几处"配置解析了但没接线"和"错误路径没有闭环"的问题。以下每一条都有对应的回归测试：

| # | 问题 | 修复 | 测试 |
|---|------|------|------|
| 1 | `restart_delay` 解析了但从不使用，重启间隔只等于 `check_interval` | 真正生效，2^n 指数退避 | `test_restart_backoff` |
| 2 | `stopAll()` 直接 `force=true`，子进程收不到 SIGTERM | 先 SIGTERM 等 `graceful_timeout`，超时才 SIGKILL | `test_graceful_shutdown` |
| 3 | `check_interval = 0` 使等待循环变成忙等，CPU 99% | 钳位到 1..3600 并告警；改用条件变量等待 | `test_check_interval_clamp` |
| 4 | `ProcessConfig` 的 int 字段无默认值，缺键即读未初始化内存 | 所有字段就地初始化 | `test_config_defaults` |
| 5 | 重启计数终生累计，偶发崩溃耗尽预算后静默放弃 | `restart_reset_after` 健康运行后重置 | `test_restart_budget_reset` |
| 5b | 首次启动被算作一次重启，`max_restarts=0` 一次都不启动 | 首次启动单独放行 | `test_zero_restarts` |
| 5c | pid 映射用显示名、查找用节名，两者不同时停止失效 | 统一以配置节名为键 | `test_name_alias` |
| 5d | `is_running` 在锁外置位，可能被 reaper 的 false 覆盖 | 状态翻转与 pid 登记同锁完成 | 全部重启类测试 |
| 6 | `fork` 后子进程里调用了非异步信号安全函数（含加锁） | `fork` 前准备 argv/envp，子进程只调安全函数 | TSan 无告警 |
| 7 | `exec` 失败只在子进程 stderr 打印，父进程判"成功" | CLOEXEC 管道回传 errno | `test_exec_failure` |
| 8 | `waitpid` 拿到 status 后丢弃，退出码从不落日志 | 记录退出码/信号/uptime | `test_exit_status_logged` |
| 9 | 用 `kill(pid, 0)` 判活，僵尸进程被误判为存活 | 单 reaper 线程统一回收，pid 表即权威状态 | `test_no_zombie` |
| 10 | 守护进程被 SIGKILL 后子进程成孤儿 | `prctl(PR_SET_PDEATHSIG, SIGTERM)` | `test_orphan_prevention` |
| 11 | 信号 handler 里 join 线程、抢锁，且 `stopAll` 被调 3 次 | `sigtimedwait` 同步消费；`stopAll` 幂等 | 全部测试 |
| 12 | 子进程继承了屏蔽的 SIGTERM，永远收不到终止信号 | `fork` 后重置信号屏蔽字与处置 | `test_graceful_shutdown` |
| 13 | 每条日志都 open/close 一次文件；`localtime` 非线程安全 | 日志流只开一次；改用 `localtime_r`；转义换行防注入 | — |
| 14 | 无 pid 文件，可重复启动 | `pid_file` + `flock` | `test_pidfile_lock` |
| 15 | 默认配置名 `process_config.ini` 与实际 `config.ini` 不符 | 统一为 `config.ini` | 全部测试 |
| 16 | `-DCOMPILER_TARGET=arm64` 仍产出 x86 二进制 | 工具链在 `project()` 之前设置 | 见第六节 |
| 17 | CMake 链接了未使用的 SQLite / `rt` / `dl` / `m` | 删除，只留 `Threads::Threads` | — |
| 18 | Windows 环境块用 `std::string + "\0"` 拼装（不产生 NUL 分隔符） | 改用 `std::vector<char>` 显式拼 NUL | **未在 Windows 验证** |
| 19 | Windows 每次重启泄漏一个 Job 句柄；`process_jobs_` 无锁写入 | 替换旧 Job 前先 `CloseHandle`；写入加锁 | **未在 Windows 验证** |

### 验证范围

- **Linux：已实机验证。** 12 条测试在 Debug、ASan、TSan 三种构建下全部通过；arm64 与 arm32 交叉编译产物经 `file` 确认架构正确。
- **Windows：未验证。** `#ifdef _WIN32` 分支只修掉了上表第 18、19 条确认的缺陷，**没有在 Windows 上编译或运行过**。请勿据此认为 Windows 分支可用。

---

## 十、已知限制

按"收益 ÷ 工作量"排序，这些是**明确没做**的：

### 功能层面

- **没有健康检查**。当前只有"进程存在"这一种判定。程序活着但卡死（比如死锁、无限循环）是发现不了的。需要的话得自己加端口探针或心跳文件
- **没有热重载**。改了配置必须整体重启，连带把所有健康进程一起打断
- **没有单进程控制**。不能只重启其中一个，也没有 `status` 命令
- **没有日志轮转**。日志文件会一直长，得靠外部工具（logrotate）处理
- **没有结构化日志**。只有纯文本，不好接 ELK/Loki 之类的系统

### 架构层面

- **`Process` 是个上帝类**。配置解析、进程控制、日志、字符串工具全在一个类里，`process.cpp` 有 1500 行。想改一块得先读懂全部
- **线程模型是 2N**（每进程 1 监控 + 共享 1 收尸）。进程数上百时，每秒几百次无谓唤醒。可以换成 `pidfd` / `signalfd` / `timerfd` 单事件循环
- **平台代码靠 `#ifdef` 切割**。想加第三个平台会很痛苦。更好的做法是抽一个 `IProcessBackend` 接口
- **锁粒度偏粗**。`state_mutex_` 保护了所有进程的 pid 表，一个进程的查询会阻塞另一个

### 可观测性

- 没有 metrics 接口
- 没有状态文件（外部想知道"现在管着哪些进程、各自什么状态"只能读日志）
- 日志里没有线程名，多线程排查时不好区分

---

## 许可证

MIT，见 [LICENSE](LICENSE)。
