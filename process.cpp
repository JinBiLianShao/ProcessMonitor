/**
 * @file process.cpp
 * @brief ProcessMonitor 的全部实现。看着挺长，其实就四块：
 *        配置解析 / 启停进程 / 监控重启 / 日志。
 * @author jinbilianshao
 *
 * 这一版修掉的坑，按坑的深度排：
 *
 *  1. restart_delay 以前是解析了但从来没用 —— 配置里写着"延迟 10 秒重启"，
 *     实际每 check_interval 就拉一次。现在是真退避，2 的 n 次方往上翻。
 *  2. 停止时直接 SIGKILL。graceful_timeout 压根没生效，子进程连收拾后事的机会都没有。
 *  3. check_interval = 0 会让等待循环跑 0 次，整个 while 变成纯忙等，CPU 直接 99%。
 *  4. ProcessConfig 的 int 字段没默认值，配置文件漏写就是未初始化内存。
 *  5. 重启计数只增不减，跑了三个月的进程偶发崩几次就永久"放弃"了，而且不吭声。
 *  6. fork 之后在子进程里调了加锁和 iostream —— 父进程多线程时可能永久死锁。
 *  7. exec 失败只在子进程 stderr 打一行，父进程还以为启动成功了。
 *  8. waitpid 拿到 status 就扔了，退出码从来不记。
 *  9. 用 kill(pid, 0) 判活 —— 僵尸进程也能返回 0，等于判断"僵尸还活着"。
 * 10. 守护进程被 SIGKILL 之后，子进程全变孤儿继续跑。
 * 11. 信号 handler 里 join 线程、抢锁。
 * 12. 子进程继承了父进程屏蔽的 SIGTERM，永远收不到终止信号。
 *
 * 每一条在 tests/ 下都有对应的 CTest，改代码之前先跑一遍。
 *
 * Windows 那半边顺手修了三个已知缺陷（环境块拼装、Job 句柄泄漏、无锁写 map），
 * 但【没在 Windows 上编译运行过】，别当它能用。
 */

#include "process.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
extern char **environ;
#endif

namespace {

// check_interval 的合法区间。下限是 1 秒不是 0 —— 0 会忙等，负数更离谱。
constexpr int kMinCheckInterval = 1;
constexpr int kMaxCheckInterval = 3600;

// reaper 多久看一次。100ms 对"进程退出"这种事件足够了，
// 再快就是浪费 CPU，再慢子进程变僵尸的时间会变长。
constexpr int kReapPollMs = 100;

// 停机时最多等子进程被回收这么久。设这个是因为理论上存在"SIGKILL 都杀不掉"
// 的进程（比如卡在不可中断的 IO 上），不能让 stopAll 的 join 永远挂住。
constexpr int kShutdownReapGraceMs = 30000;

/**
 * 算退避延迟：base * 2^attempt，封顶 backoff_max。
 *
 * attempt 是从 0 开始数的"第几次重启"。所以第一次重启就是 base 本身，
 * 第二次翻倍，以此类推。到了上限就不再涨，免得等到天荒地老。
 */
int computeBackoff(int base_delay, int attempt, int backoff_max) {
    if (base_delay <= 0) return 0;
    // 上限配得比基数还小的话，以基数为准，不然逻辑上说不通
    if (backoff_max < base_delay) backoff_max = base_delay;

    long long delay = base_delay;
    for (int i = 0; i < attempt && delay < backoff_max; ++i) {
        delay *= 2;
    }
    if (delay > backoff_max) delay = backoff_max;
    return static_cast<int>(delay);
}

#ifndef _WIN32
/**
 * 把 waitpid 那个晦涩的 status 翻译成人话，顺便把退出码/信号拆出来给调用方。
 *
 * status 这玩意儿必须用 WIFEXITED 之类的宏去解，直接当整数看是错的。
 */
std::string describeExitStatus(int status, int &exit_code, int &term_signal) {
    std::ostringstream oss;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
        term_signal = 0;
        oss << "exited with code " << exit_code;
    } else if (WIFSIGNALED(status)) {
        exit_code = -1;
        term_signal = WTERMSIG(status);
        oss << "killed by signal " << term_signal << " (" << strsignal(term_signal) << ")";
    } else {
        // 理论上到不了这儿，到了就说明内核给了个我们不认识的状态
        exit_code = -1;
        term_signal = 0;
        oss << "ended with unexpected status " << status;
    }
    return oss.str();
}
#endif

} // namespace

Process::Process() {
    log(LogLevel::INFO, "Process Daemon Initialized");
}

Process::~Process() {
    // stopAll 幂等，正常流程里 main 已经调过了，这里再调就是空转
    stopAll();
#ifdef _WIN32
    // Windows 的 Job 句柄要自己收，不然进程退出时句柄表泄漏
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto &pair : process_jobs_) {
        if (pair.second != nullptr && pair.second != INVALID_HANDLE_VALUE) {
            CloseHandle(pair.second);
        }
    }
    process_jobs_.clear();
#endif
}
// ============================ 配置解析 ============================
//
// 手写的 INI 解析。没上第三方库是因为这格式简单得没必要，
// 而且多一个依赖就多一个交叉编译时要伺候的东西。
//
// 支持的语法：
//   [Section]          节名
//   key = value        键值对，等号两边空白会被吃掉
//   # 注释             整行以 # 或 ; 开头
//
// 不支持的（别指望）：
//   行尾注释、值里带逗号的 env_vars、跨行续行、节名继承

/**
 * 解析一个整数配置项。
 *
 * 这个函数是"配置项写错不许静默"这个原则的落地点。三种情况：
 *   - 空值        -> 打 WARNING，保留默认值
 *   - 不是数字    -> 打 WARNING，保留默认值（含 "123abc" 这种半截数字）
 *   - 超出范围    -> 打 WARNING，钳到边界上（比如 check_interval = 0 会被钳到 1）
 *
 * 返回值只是告诉调用方"解出来没有"，方便调用方决定要不要额外处理。
 * out 参数在失败时保持原值，所以外面可以放心传默认值进来。
 */
bool Process::parseIntSetting(const std::string &section, const std::string &key,
                              const std::string &raw, int min_value, int max_value, int &out) {
    if (raw.empty()) {
        log(LogLevel::WARNING, "Empty value for " + key + " in section [" + section +
                               "], keeping default " + std::to_string(out));
        return false;
    }
    try {
        size_t consumed = 0;
        long long parsed = std::stoll(raw, &consumed);
        // stoll 遇到 "123abc" 只会读 123 然后停下，不检查 consumed 就等于默默接受垃圾
        if (consumed != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        if (parsed < min_value || parsed > max_value) {
            log(LogLevel::WARNING, "Value out of range for " + key + " in section [" + section +
                                   "]: " + raw + " (allowed " + std::to_string(min_value) + ".." +
                                   std::to_string(max_value) + "), clamping");
            parsed = std::min<long long>(std::max<long long>(parsed, min_value), max_value);
        }
        out = static_cast<int>(parsed);
        return true;
    } catch (const std::exception &) {
        log(LogLevel::WARNING, "Invalid value for " + key + " in section [" + section + "]: " + raw +
                               ", keeping default " + std::to_string(out));
        return false;
    }
}

/**
 * 读配置文件。
 *
 * 流程：逐行扫 -> 遇到 [Xxx] 就开个新进程条目（[Global] 除外）-> 遇到 key=value
 * 就按当前节去填。读完统一做校验，配置不合法（比如 path 不存在）的条目直接踢掉。
 *
 * 几个刻意的设计：
 *   - 未知的键打 WARNING 而不是忽略。拼错一个字母查半天太亏。
 *   - 显示名重复也打 ERROR，否则日志里两个进程一个名字，根本分不清。
 *   - 返回 false 的条件是"一个能用的进程都没有"，而不是"文件打不开"。
 */
bool Process::loadConfig(const std::string &config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        log(LogLevel::ERROR, "Cannot open config file: " + config_file +
                             " (errno: " + std::to_string(errno) + ")");
        return false;
    }

    std::string line;
    std::string current_section;
    bool saw_global = false;

    // 去首尾空白。用 lambda 是因为下面要调用十几次，写个函数省事。
    auto trim = [](const std::string &s) {
        const auto begin = s.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return std::string();
        const auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(begin, end - begin + 1);
    };

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        // ---- 节头 [Xxx] ----
        if (line.front() == '[' && line.back() == ']') {
            current_section = trim(line.substr(1, line.size() - 2));
            if (current_section == "Global") {
                saw_global = true;
            } else if (current_section.empty()) {
                log(LogLevel::WARNING, "Ignoring config section with empty name");
            } else {
                // 每个非 Global 的节就是一个要托管的进程。名字默认取节名，
                // 后面如果读到 name = xxx 再覆盖。
                auto inserted = processes_.emplace(current_section, ProcessConfig{});
                if (inserted.second) {
                    inserted.first->second.name = current_section;
                }
            }
            continue;
        }

        // ---- 键值对 ----
        const size_t pos = line.find('=');
        if (pos == std::string::npos) {
            log(LogLevel::WARNING, "Ignoring malformed config line: " + line);
            continue;
        }

        const std::string key = trim(line.substr(0, pos));
        const std::string value = trim(line.substr(pos + 1));

        if (current_section == "Global") {
            // ---- 全局配置 ----
            if (key == "log_level") {
                if (value == "DEBUG") log_level_ = LogLevel::DEBUG;
                else if (value == "INFO") log_level_ = LogLevel::INFO;
                else if (value == "WARNING") log_level_ = LogLevel::WARNING;
                else if (value == "ERROR") log_level_ = LogLevel::ERROR;
                else log(LogLevel::WARNING, "Unknown log_level: " + value + ", keeping INFO");
            } else if (key == "log_file") {
                log_file_ = value;
            } else if (key == "check_interval") {
                int parsed = check_interval_;
                if (parseIntSetting(current_section, key, value, kMinCheckInterval, kMaxCheckInterval, parsed)) {
                    check_interval_ = parsed;
                } else {
                    // 解析失败也兜个底：宁可 1 秒也不要 0 秒（0 会忙等烧 CPU）
                    check_interval_ = std::max(check_interval_, kMinCheckInterval);
                }
            } else if (key == "pid_file") {
                pid_file_ = value;
            } else {
                log(LogLevel::WARNING, "Unknown key in [Global]: " + key);
            }
            continue;
        }

        if (current_section.empty()) {
            log(LogLevel::WARNING, "Config key outside any section: " + key);
            continue;
        }

        // ---- 进程配置 ----
        ProcessConfig &config = processes_[current_section];
        if (key == "name") {
            // 空值就当没写，别把进程名清成空串
            if (!value.empty()) config.name = value;
        } else if (key == "path") {
            config.path = value;
        } else if (key == "working_dir") {
            config.working_dir = value;
        } else if (key == "args") {
            config.args = value;
        } else if (key == "env_vars") {
            config.env_vars = parseEnvVars(value);
        } else if (key == "restart_delay") {
            parseIntSetting(current_section, key, value, 0, 86400, config.restart_delay);
        } else if (key == "restart_backoff_max") {
            parseIntSetting(current_section, key, value, 0, 86400, config.restart_backoff_max);
        } else if (key == "max_restarts") {
            // 下限是 -1，因为 -1 表示"无限重启"
            parseIntSetting(current_section, key, value, -1, 1000000, config.max_restarts);
        } else if (key == "graceful_timeout") {
            parseIntSetting(current_section, key, value, 0, 3600, config.graceful_timeout);
        } else if (key == "restart_reset_after") {
            parseIntSetting(current_section, key, value, 0, 86400, config.restart_reset_after);
        } else {
            log(LogLevel::WARNING, "Unknown key in [" + current_section + "]: " + key);
        }
    }

    if (!saw_global) {
        log(LogLevel::WARNING, "Config has no [Global] section, using defaults");
    }

    // 校验：path 不存在之类的直接踢掉，别等到 startAll 才发现
    for (auto it = processes_.begin(); it != processes_.end();) {
        if (!validateProcessConfig(it->second)) {
            log(LogLevel::ERROR, "Invalid configuration for process: " + it->first);
            it = processes_.erase(it);
        } else {
            ++it;
        }
    }

    // 显示名查重。允许重名，但日志会分不清是谁，所以至少提醒一下。
    std::map<std::string, std::string> display_names;
    for (const auto &pair : processes_) {
        auto inserted = display_names.emplace(pair.second.name, pair.first);
        if (!inserted.second) {
            log(LogLevel::ERROR, "Duplicate process name '" + pair.second.name + "' in sections [" +
                                 inserted.first->second + "] and [" + pair.first +
                                 "]; logs will be ambiguous");
        }
    }

    log(LogLevel::INFO, "Loaded " + std::to_string(processes_.size()) + " process configurations");
    return !processes_.empty();
}
// ============================ 生命周期 ============================

/**
 * 起线程。
 *
 * 顺序有讲究：先拿 pid 文件（拿不到说明已经有一个实例在跑了，直接撤），
 * 再起 reaper，最后起各个监控线程。reaper 必须先于监控线程起来，
 * 否则第一批子进程退出时没人收尸。
 */
bool Process::startAll() {
    if (processes_.empty()) {
        log(LogLevel::ERROR, "No process to monitor");
        return false;
    }

    global_stop_ = false;
    stopping_ = false;

    for (const auto &pair : processes_) {
        // ProcessRuntime 里有 atomic，不能拷贝也不能移动，只能就地默认构造
        runtimes_[pair.first];
    }

    if (!pid_file_.empty() && !acquirePidFile(pid_file_)) {
        return false;
    }

#ifndef _WIN32
    reaper_thread_ = std::thread(&Process::reaperLoop, this);
#endif

    for (const auto &pair : processes_) {
        const std::string &key = pair.first;
        monitor_threads_[key] = std::thread(&Process::monitorProcess, this, key);
        log(LogLevel::INFO, "Started monitor for: " + pair.second.name);
    }

    return true;
}

/**
 * 停一切。顺序不能乱：
 *
 *   1) 拉总闸 global_stop_，唤醒所有在等的线程。监控线程看到就退出循环，
 *      之后不会再往上拉新进程 —— 这一步必须在停止子进程之前，否则你停一个它拉一个。
 *   2) join 所有监控线程。到这儿为止不会再有新子进程了。
 *   3) 挨个优雅停止：SIGTERM -> 等 graceful_timeout -> 还赖着就 SIGKILL。
 *   4) join reaper，让它把最后几个子进程收干净。
 *
 * stopping_ 是防重入的：main 里调一次，析构函数里还会再调一次，
 * 以前没做这个，整套流程跑三遍，日志重复三份。
 */
bool Process::stopAll() {
    if (stopping_.exchange(true)) {
        return true;
    }

    log(LogLevel::INFO, "Stopping all processes...");

    // 1) 拉总闸
    global_stop_ = true;
    cv_.notify_all();

    // 2) 等监控线程退干净
    for (auto &pair : monitor_threads_) {
        if (pair.second.joinable()) {
            pair.second.join();
        }
    }
    monitor_threads_.clear();

    // 3) 优雅停止。注意 force=false —— 这里就是要走 SIGTERM 那条路。
    //    老代码传的是 true，等于永远直接 SIGKILL，graceful_timeout 白配。
    for (const auto &pair : processes_) {
        auto it = runtimes_.find(pair.first);
        if (it != runtimes_.end()) {
            it->second.should_stop = true;
        }
        stopProcess(pair.first, /*force=*/false);
    }

    // 4) 收尸
#ifndef _WIN32
    if (reaper_thread_.joinable()) {
        reaper_thread_.join();
    }
#endif

    releasePidFile();
    log(LogLevel::INFO, "All processes stopped");
    return true;
}

void Process::setLogLevel(LogLevel level) {
    log_level_ = level;
}

// ============================ 监控与重启 ============================

/**
 * 单个进程的监控循环。每个被托管的进程一个线程。
 *
 * 每一轮干三件事：看看还活着没 -> 死了就（按预算和退避）拉起来 -> 睡 check_interval。
 *
 * 关于 max_restarts 的语义，这里有个容易搞错的点：
 * 它约束的是"重启"次数，不是"启动"次数。所以首次启动永远放行 ——
 * 否则 max_restarts = 0 的进程一次都不会被拉起来，这显然不是用户想要的。
 * （0 的正确含义是"只启动，挂了就别管了"。）
 *
 * 另外 is_running 不在这里置位，而是交给 startProcess 在锁里翻。
 * 原因见 startProcess 里的注释 —— 锁外置位会被 reaper 抢跑。
 */
void Process::monitorProcess(const std::string &process_name) {
    auto cfg_it = processes_.find(process_name);
    if (cfg_it == processes_.end()) {
        log(LogLevel::ERROR, "Monitor started for unknown process: " + process_name);
        return;
    }
    const ProcessConfig &config = cfg_it->second;
    // startAll 已经给每个配置预建了运行态，这里只查不插，所以不用持锁
    ProcessRuntime &runtime = runtimes_.at(process_name);

    while (!global_stop_) {
        if (runtime.should_stop) {
            break;
        }

        if (!runtime.is_running && !runtime.gave_up) {
            const int attempts = runtime.restart_count.load();
            const bool unlimited = config.max_restarts < 0;
            const bool first_start = !runtime.started_once;

            if (first_start || unlimited || attempts < config.max_restarts) {
                // 首次启动不用等，只有"重启"才退避
                if (attempts > 0) {
                    const int delay = computeBackoff(config.restart_delay, attempts - 1,
                                                     config.restart_backoff_max);
                    if (delay > 0) {
                        log(LogLevel::INFO, "Waiting " + std::to_string(delay) + "s before restarting " +
                                            config.name);
                        // 等的时候也要能被 stopAll 打断，不然停机要干等 60 秒
                        if (!waitInterruptible(static_cast<int64_t>(delay) * 1000)) {
                            break;
                        }
                    }
                }

                log(LogLevel::WARNING, "Process " + config.name + " is not running. Restarting... (" +
                                       std::to_string(attempts + 1) + "/" +
                                       (unlimited ? std::string("inf") : std::to_string(config.max_restarts)) + ")");

                if (startProcess(process_name, config)) {
                    // is_running / started_at_ms 是 startProcess 在 state_mutex_ 里一起置的。
                    // 千万别挪到锁外面来 —— 见 startProcess 里那段注释。
                    runtime.started_once = true;
                    runtime.restart_count.fetch_add(1);
                } else {
                    // 启动失败也算一次尝试。不然遇到一个永远起不来的目标
                    // （比如路径写错），日志会被无限刷屏。
                    runtime.restart_count.fetch_add(1);
                    log(LogLevel::ERROR, "Failed to start process: " + config.name);
                }
            } else {
                runtime.gave_up = true;
                log(LogLevel::ERROR, "Max restarts reached for: " + config.name +
                                     ". Giving up; the process is left in FAILED state.");
                break;
            }
        }

        // 睡一会儿再看。用 waitInterruptible 而不是 sleep_for，
        // 这样 stopAll 一声令下就能立刻醒。
        if (!waitInterruptible(static_cast<int64_t>(check_interval_) * 1000)) {
            break;
        }
    }

    log(LogLevel::INFO, "Monitor thread stopped for: " + config.name);
}
#ifndef _WIN32

/**
 * 收尸线程。全进程只有一个。
 *
 * 为什么只有一个？因为一个子进程只能被 waitpid 一次。多个线程同时 waitpid(-1)
 * 会互相抢，谁抢到算谁的，结果就是某个进程的退出码丢了、重启计数乱了。
 *
 * 这个线程同时维护着"谁还活着"的真相：pid 从 process_pids_ 里删掉就代表
 * 那个进程真的没了。别的代码要判活就查这张表，不要自己 kill(pid, 0)。
 *
 * 停机阶段要特殊处理：global_stop_ 一旦置位，waitInterruptible 会立刻返回，
 * 这里就会变成空转，所以停机时改用普通 sleep。另外还有个 30 秒的安全阀，
 * 防止某个杀不掉的进程把 join 永久卡住。
 */
void Process::reaperLoop() {
    std::chrono::steady_clock::time_point shutdown_deadline{};
    while (true) {
        reapAllChildren();

        if (global_stop_) {
            bool any_alive = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                any_alive = !process_pids_.empty();
            }
            if (!any_alive) {
                break; // 收干净了，可以下班
            }
            // 安全阀：理论上 SIGKILL 是不可忽略的，但真遇到卡在不可中断 IO 的进程，
            // 也只能放弃，总比让 stopAll 的 join 永远挂着强。
            if (shutdown_deadline == std::chrono::steady_clock::time_point{}) {
                shutdown_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(kShutdownReapGraceMs);
            } else if (std::chrono::steady_clock::now() >= shutdown_deadline) {
                size_t stuck = 0;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    stuck = process_pids_.size();
                }
                log(LogLevel::ERROR, "Reaper giving up: " + std::to_string(stuck) +
                                     " child process(es) still not reapable after shutdown grace period");
                break;
            }
            // 停机时 global_stop_ 已经是 true，用 waitInterruptible 会立刻返回 -> 空转
            std::this_thread::sleep_for(std::chrono::milliseconds(kReapPollMs));
            continue;
        }

        waitInterruptible(kReapPollMs);
    }
    reapAllChildren();
}

/**
 * 把当前所有已退出的子进程收一遍。
 *
 * 循环是因为一次可能有好几个同时挂了。WNOHANG 表示没有就立刻返回，
 * 不要阻塞在这儿 —— 这是个轮询线程，不是 wait 线程。
 */
void Process::reapAllChildren() {
    while (true) {
        int status = 0;
        const pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            handleChildExit(pid, status);
            continue;
        }
        // 被信号打断了，重来
        if (pid == -1 && errno == EINTR) {
            continue;
        }
        // pid == 0（还有子进程没退）或者 ECHILD（没有子进程了），都该收手
        break;
    }
}

/**
 * 处理"某个子进程没了"这件事。
 *
 * 关键点：状态翻转（删 pid、置 is_running=false）必须和 startProcess 里
 * 的登记操作在同一把锁里完成。不然会出现这种竞态：
 *   监控线程：startProcess 返回，准备置 is_running = true
 *   reaper：  子进程秒退，先置了 is_running = false
 *   监控线程：置 is_running = true  <- 覆盖了，从此认为进程一直在跑，再也不重启
 * 这个 bug 极难复现，但只要出现就是"进程死了但守护进程不管"。
 */
void Process::handleChildExit(pid_t pid, int status) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto it = process_pids_.begin(); it != process_pids_.end(); ++it) {
            if (it->second == pid) {
                name = it->first;
                process_pids_.erase(it);
                break;
            }
        }
        if (name.empty()) {
            return; // 不认识的 pid，不是我们托管的东西，不管
        }
        process_groups_.erase(name);
        auto rt_it = runtimes_.find(name);
        if (rt_it != runtimes_.end()) {
            rt_it->second.is_running = false;
        }
    }

    auto cfg_it = processes_.find(name);
    auto rt_it = runtimes_.find(name);
    if (cfg_it == processes_.end() || rt_it == runtimes_.end()) {
        return;
    }

    const ProcessConfig &config = cfg_it->second;
    ProcessRuntime &runtime = rt_it->second;

    int exit_code = -1;
    int term_signal = 0;
    const std::string how = describeExitStatus(status, exit_code, term_signal);

    runtime.last_exit_code = exit_code;
    runtime.last_term_signal = term_signal;

    // 算一下活了多久。这个数字在排查"为什么老挂"时非常有用：
    // 跑 0 秒就死通常是配置/依赖问题，跑几小时才死通常是内存泄漏。
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    const int64_t started_at_ms = runtime.started_at_ms.load();
    const int64_t uptime_ms = (started_at_ms > 0 && now_ms > started_at_ms) ? now_ms - started_at_ms : 0;

    if (runtime.should_stop || global_stop_) {
        // 是我们让它停的，属于正常流程，用 INFO
        log(LogLevel::INFO, "Process " + config.name + " (PID " + std::to_string(pid) + ") " + how +
                            " after " + std::to_string(uptime_ms / 1000) + "s (stopping)");
        return;
    }

    // 不是我们让它停的，那就是意外，用 WARNING
    log(LogLevel::WARNING, "Process " + config.name + " (PID " + std::to_string(pid) + ") " + how +
                           " after " + std::to_string(uptime_ms / 1000) + "s");

    // 活了够久就说明它其实是健康的，把重启计数清零。
    // 否则一个跑三个月才崩一次的进程，攒够 5 次就永久放弃了。
    if (config.restart_reset_after > 0 && uptime_ms >= static_cast<int64_t>(config.restart_reset_after) * 1000) {
        const int previous = runtime.restart_count.exchange(0);
        if (previous > 0) {
            log(LogLevel::INFO, "Process " + config.name + " ran for " +
                                std::to_string(uptime_ms / 1000) + "s (>= restart_reset_after=" +
                                std::to_string(config.restart_reset_after) + "), resetting restart budget (" +
                                std::to_string(previous) + " -> 0)");
        }
    }
}

#else

// Windows 没有 fork/waitpid 这一套，收尸靠句柄轮询。
// 这三个留空是为了让上层代码不用 #ifdef 满天飞。
void Process::reaperLoop() {}
void Process::reapAllChildren() {}
void Process::handleChildExit(pid_t, int) {}

#endif // !_WIN32
// ============================ 进程控制 ============================

/**
 * 启动一个进程。
 *
 * @param key    配置节名。存 pid 表用这个，因为显示名可能和节名不一样。
 * @param config 那份配置。打日志用 config.name。
 *
 * 为什么要传两个？因为 name 键允许改显示名。曾经踩过的坑：
 * 存表用显示名、查找用节名，只要用户写了 name = xxx，停止流程就彻底失效，
 * 子进程根本收不到 SIGTERM。
 */
bool Process::startProcess(const std::string &key, const ProcessConfig &config) {
    const std::string resolved_path = resolvePath(config.path, config.working_dir);
    std::string resolved_working_dir = config.working_dir;
    if (resolved_working_dir.empty()) {
        // 没配工作目录就默认用可执行文件所在的目录，这是最符合直觉的选择
        const size_t slash = resolved_path.find_last_of("/\\");
        resolved_working_dir = (slash == std::string::npos) ? "." : resolved_path.substr(0, slash);
    }

    if (!fileExists(resolved_path)) {
        log(LogLevel::ERROR, "Executable not found: " + resolved_path);
        return false;
    }

    // 工作目录建不出来不算致命，打 WARNING 继续 —— 也许人家是只读挂载点
    if (!createDirectory(resolved_working_dir)) {
        log(LogLevel::WARNING, "Cannot create working directory: " + resolved_working_dir);
    }

#ifdef _WIN32
    PROCESS_INFORMATION proc_info;
    if (!createWindowsProcess(key, config, proc_info)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        process_handles_[key] = proc_info;
    }
    return true;
#else
    pid_t pid = -1;
    if (!createUnixProcess(config, pid)) {
        return false;
    }
    {
        // 这段锁里的东西一个都不能挪出去：
        //
        // a) 用 key（节名）当键，不是 config.name（显示名）。
        // b) is_running 和 started_at_ms 必须在这里一起置位。
        //    如果先登记 pid、再在锁外置 is_running，一个启动后立刻退出的子进程
        //    会被 reaper 抢先置成 false，然后被这里覆盖成 true —— 于是守护进程
        //    永远认为它活着，再也不会重启它。这个 bug 只在进程秒退时出现，
        //    测试里专门有两条盯着（test_no_zombie / test_zero_restarts）。
        std::lock_guard<std::mutex> lock(state_mutex_);
        process_pids_[key] = pid;
        process_groups_[key] = pid; // setsid 之后，子进程自己就是进程组组长
        auto rt_it = runtimes_.find(key);
        if (rt_it != runtimes_.end()) {
            rt_it->second.is_running = true;
            rt_it->second.started_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::system_clock::now().time_since_epoch())
                                                  .count();
        }
    }
    log(LogLevel::INFO, "Started Unix process: " + config.name + " PID: " + std::to_string(pid));
    return true;
#endif
}

/**
 * 停止一个进程。
 *
 * @param force true = 直接 SIGKILL，不给收拾后事的机会
 *
 * 注意这里只在锁里查一下 pid/进程组号，然后就把锁放了。
 * 因为 terminateUnixProcess 会等子进程退出（最长 graceful_timeout 秒），
 * 拿着锁等的话，整个守护进程的所有状态查询全都被堵住。
 */
bool Process::stopProcess(const std::string &process_name, bool force) {
    auto cfg_it = processes_.find(process_name);
    const int graceful = (cfg_it != processes_.end()) ? cfg_it->second.graceful_timeout : 0;

#ifdef _WIN32
    PROCESS_INFORMATION proc_info;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = process_handles_.find(process_name);
        if (it == process_handles_.end()) {
            return false;
        }
        proc_info = it->second;
    }
    return terminateWindowsProcess(process_name, proc_info, force ? 0 : graceful * 1000);
#else
    pid_t pid = -1;
    pid_t group = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it_pid = process_pids_.find(process_name);
        if (it_pid == process_pids_.end()) {
            return false;
        }
        pid = it_pid->second;
        auto it_group = process_groups_.find(process_name);
        group = (it_group != process_groups_.end()) ? it_group->second : pid;
    }
    // 锁已释放，这里可以慢慢等
    return terminateUnixProcess(pid, group, force ? 0 : graceful);
#endif
}

/**
 * 配置校验。主要就两件事：路径得存在，数值不能是负的。
 *
 * 数值的合法性在 parseIntSetting 里已经钳过一遍了，这里再查是为了
 * 兜住"直接构造 ProcessConfig 传进来"的情况。
 */
bool Process::validateProcessConfig(const ProcessConfig &config) {
    if (config.path.empty()) {
        log(LogLevel::ERROR, "Process path is empty for: " + config.name);
        return false;
    }

    const std::string resolved_path = resolvePath(config.path, config.working_dir);
    if (!fileExists(resolved_path)) {
        log(LogLevel::ERROR, "Executable does not exist: " + resolved_path);
        return false;
    }

    if (config.restart_delay < 0) {
        log(LogLevel::ERROR, "Invalid restart delay for: " + config.name);
        return false;
    }
    if (config.max_restarts < -1) {
        log(LogLevel::ERROR, "Invalid max restarts for: " + config.name);
        return false;
    }
    if (config.graceful_timeout < 0) {
        log(LogLevel::ERROR, "Invalid graceful timeout for: " + config.name);
        return false;
    }
    return true;
}
// ============================ Unix 实现 ============================

#ifndef _WIN32

namespace {

/**
 * 拼 execve 要的环境变量数组。
 *
 * 语义是"继承父进程的环境，再把配置里的覆盖/追加进去"。
 * 不是"只用配置里的"—— 那样会丢掉 PATH、HOME 这些子进程活不下去的东西。
 */
std::vector<std::string> buildEnvironment(const std::map<std::string, std::string> &overrides) {
    std::vector<std::string> result;

    if (environ != nullptr) {
        for (char **entry = environ; *entry != nullptr; ++entry) {
            const std::string kv(*entry);
            const size_t eq = kv.find('=');
            const std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
            if (overrides.find(key) != overrides.end()) {
                continue; // 这个键配置里要覆盖，跳过父进程的版本
            }
            result.push_back(kv);
        }
    }

    for (const auto &pair : overrides) {
        result.push_back(pair.first + "=" + pair.second);
    }
    return result;
}

} // namespace

/**
 * fork + execve 起一个子进程。整个文件里最需要小心的函数。
 *
 * ================== 核心约束：fork 之后只能干安全的事 ==================
 *
 * 父进程是多线程的。fork 出来的子进程只复制了【调用 fork 的那个线程】，
 * 但所有的锁状态是原样复制的。也就是说：如果 fork 的那一刻别的线程正拿着
 * log_mutex_，子进程里的这把锁就永远是锁着的 —— 子进程再去 log() 就是
 * 自己锁自己，卡死在那儿，连 exec 都到不了。
 *
 * 所以子进程里只允许调用"异步信号安全"的函数：
 *   sigprocmask / sigaction / setsid / prctl / chdir / execve / write / _exit
 * 绝对不许碰：std::string、iostream、malloc、任何 mutex。
 *
 * 因此 argv 和 envp 全部在 fork 之前就拼好，子进程只负责 execve。
 *
 * ================== exec 结果怎么告诉父进程 ==================
 *
 * 父进程不能干等（不知道子进程会不会 exec 成功），所以用一根管道：
 *   子进程 execve 成功 -> 管道的写端带 CLOEXEC，exec 时被内核自动关掉
 *                     -> 父进程读到 EOF，说明成功了
 *   子进程 execve 失败 -> 把 errno 写进管道
 *                     -> 父进程读到一个 int，说明失败了，而且知道为什么
 *
 * 以前没有这套握手，exec 失败只在子进程 stderr 打一行就完了，父进程照样
 * 报"启动成功"、照样消耗重启预算，日志里还显示一个根本不存在的 PID。
 */
bool Process::createUnixProcess(const ProcessConfig &config, pid_t &pid) {
    const std::string resolved_path = resolvePath(config.path, config.working_dir);
    std::string resolved_working_dir = config.working_dir;
    if (resolved_working_dir.empty()) {
        const size_t slash = resolved_path.find_last_of('/');
        resolved_working_dir = (slash == std::string::npos) ? "." : resolved_path.substr(0, slash);
    }

    // ---- 以下全部在 fork 之前完成 ----
    std::vector<std::string> arg_storage;
    arg_storage.push_back(resolved_path);
    for (const auto &arg : splitArgs(config.args)) {
        arg_storage.push_back(arg);
    }
    std::vector<char *> argv;
    argv.reserve(arg_storage.size() + 1);
    for (auto &item : arg_storage) {
        argv.push_back(const_cast<char *>(item.c_str()));
    }
    argv.push_back(nullptr); // execve 要 NULL 结尾

    std::vector<std::string> env_storage = buildEnvironment(config.env_vars);
    std::vector<char *> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto &item : env_storage) {
        envp.push_back(const_cast<char *>(item.c_str()));
    }
    envp.push_back(nullptr);

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        log(LogLevel::ERROR, "pipe() failed for " + config.name + " (errno: " + std::to_string(errno) + ")");
        return false;
    }
    // 写端必须带 CLOEXEC，这是整个握手能成立的关键
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) != 0) {
        log(LogLevel::WARNING, "fcntl(CLOEXEC) failed for " + config.name);
    }

    pid = fork();
    if (pid == -1) {
        log(LogLevel::ERROR, "Fork failed for process: " + config.name +
                             " (errno: " + std::to_string(errno) + ")");
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // ================== 子进程。只用异步信号安全的函数 ==================
        close(pipefd[0]);

        // 必须清掉继承来的信号屏蔽字。
        // 主线程为了用 sigtimedwait 同步收信号，把 SIGTERM/SIGINT/SIGQUIT 屏蔽了，
        // 这个屏蔽字会被 fork 继承 —— 不清掉的话子进程永远收不到 SIGTERM，
        // 优雅关闭就成了摆设（实测就是这个问题，test_graceful_shutdown 抓出来的）。
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        (void) sigprocmask(SIG_SETMASK, &empty_mask, nullptr);

        // 信号处置也要还原。父进程把 SIGPIPE 设成忽略了，子进程不该跟着继承。
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        (void) sigaction(SIGPIPE, &dfl, nullptr);
        (void) sigaction(SIGTERM, &dfl, nullptr);
        (void) sigaction(SIGINT, &dfl, nullptr);

        // 新会话 + 新进程组。好处是 kill(-pgid) 能一锅端掉整个进程树，
        // 坏处是它脱离了终端，Ctrl+C 影响不到它（这正是我们要的）。
        if (setsid() == -1) {
            const int err = errno;
            (void) !write(pipefd[1], &err, sizeof(err));
            _exit(127);
        }

#ifdef __linux__
        // 让内核盯着：父进程一死（哪怕是被 SIGKILL），就给子进程发 SIGTERM。
        // 没有这个的话，守护进程被 kill -9 之后子进程全成孤儿继续跑。
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
            // 失败了不算致命，只是失去孤儿保护能力
        }
        // 竞态检查：fork 和 prctl 之间父进程可能已经死了，
        // 这种情况下 PDEATHSIG 不会再触发，得自己退出。
        if (getppid() == 1) {
            _exit(127);
        }
#endif

        if (!resolved_working_dir.empty() && chdir(resolved_working_dir.c_str()) != 0) {
            const int err = errno;
            (void) !write(pipefd[1], &err, sizeof(err));
            _exit(127);
        }

        // 从这里开始，这个进程就是用户要的那个程序了
        execve(resolved_path.c_str(), argv.data(), envp.data());

        // 能走到这儿说明 execve 失败了。把 errno 告诉父进程，然后自己去死。
        // 注意不能用 exit()：它会跑 atexit 和刷新 stdio buffer，在多线程 fork 场景下不安全。
        const int err = errno;
        (void) !write(pipefd[1], &err, sizeof(err));
        _exit(127);
    }

    // ================== 父进程 ==================
    close(pipefd[1]);

    int child_errno = 0;
    ssize_t received = 0;
    do {
        received = read(pipefd[0], &child_errno, sizeof(child_errno));
    } while (received == -1 && errno == EINTR);
    close(pipefd[0]);

    if (received == static_cast<ssize_t>(sizeof(child_errno))) {
        // 读到一个 errno = exec 失败。把那个短命的子进程收掉，然后判失败。
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
        log(LogLevel::ERROR, "Failed to exec " + resolved_path + " for " + config.name +
                             ": " + strerror(child_errno) + " (errno: " + std::to_string(child_errno) + ")");
        return false;
    }

    // 读到 EOF = 写端被 exec 关掉了 = 子进程已经成功变成目标程序
    return true;
}

/**
 * 终止一个进程：先礼后兵。
 *
 * timeout_sec > 0 时：给整个进程组发 SIGTERM，然后每秒看十次它退没退。
 * 到时间还不走：SIGKILL 整个进程组，再给 5 秒收尾。
 *
 * 为什么打整个进程组而不是单个 pid？因为用户程序自己可能又 fork 了子进程，
 * 只杀父进程会留下一堆孙子辈的孤儿。
 */
bool Process::terminateUnixProcess(pid_t pid, pid_t process_group, int timeout_sec) {
    if (timeout_sec > 0) {
        log(LogLevel::INFO, "Sending SIGTERM to PID " + std::to_string(pid) +
                            " (graceful timeout " + std::to_string(timeout_sec) + "s)");
        // 负数 pid 表示"发给整个进程组"。ESRCH 说明进程已经没了，不算错。
        if (kill(-process_group, SIGTERM) == -1 && errno != ESRCH) {
            log(LogLevel::WARNING, "Failed to send SIGTERM to process group " +
                                   std::to_string(process_group) + ": " + strerror(errno));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!isUnixProcessRunning(pid)) {
                log(LogLevel::INFO, "Process " + std::to_string(pid) + " terminated gracefully");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        log(LogLevel::WARNING, "Process " + std::to_string(pid) + " did not exit within " +
                               std::to_string(timeout_sec) + "s");
    }

    // 到这儿就是要么 force=true，要么优雅等待超时了
    log(LogLevel::WARNING, "Force terminating process group: " + std::to_string(process_group));
    if (kill(-process_group, SIGKILL) == -1 && errno != ESRCH) {
        log(LogLevel::ERROR, "Failed to send SIGKILL to process group " + std::to_string(process_group) +
                             ": " + strerror(errno));
    }
    // 保险起见单发一次：万一进程组号不对，至少 pid 是对的
    if (kill(pid, SIGKILL) == -1 && errno != ESRCH) {
        log(LogLevel::ERROR, "Failed to send SIGKILL to " + std::to_string(pid) + ": " + strerror(errno));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!isUnixProcessRunning(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/**
 * 判断进程还活着没。
 *
 * 千万别改成 kill(pid, 0) == 0 —— 这个判断对僵尸进程也返回 0。
 * 子进程已经死了但还没被 wait 掉的时候，它就是个僵尸，kill 照样"成功"，
 * 于是你会认为它还活着，然后永远等下去。
 *
 * 正确做法是查 reaper 维护的那张表：进程被回收的那一刻就从表里删掉了，
 * 所以"在表里" == "真的还活着"。这张表就是唯一真相。
 */
bool Process::isUnixProcessRunning(pid_t pid) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (const auto &pair : process_pids_) {
        if (pair.second == pid) {
            return true;
        }
    }
    return false;
}
#else
// ============================ Windows 实现 ============================
//
// 警告：这段代码没在 Windows 上编译过、更没运行过。
// 只修了评审时肉眼能确认的三个缺陷（见下），其余逻辑保持原样。
// 谁要用请自己先跑通，别假设它是对的。

bool Process::createWindowsProcess(const std::string &key, const ProcessConfig &config,
                                  PROCESS_INFORMATION &proc_info) {
    STARTUPINFOA startup_info;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&proc_info, sizeof(proc_info));

    const std::string resolved_path = resolvePath(config.path, config.working_dir);
    std::string resolved_working_dir = config.working_dir;
    if (resolved_working_dir.empty()) {
        const size_t slash = resolved_path.find_last_of("/\\");
        resolved_working_dir = (slash == std::string::npos) ? "." : resolved_path.substr(0, slash);
    }

    std::string command_line = "\"" + resolved_path + "\"";
    if (!config.args.empty()) {
        command_line += " " + config.args;
    }

    // 环境块必须是 "K=V\0K=V\0\0" 这种形式（双 NUL 结尾）。
    // 老代码写的是 std::string + "\0"，而 operator+(const char*) 遇到 "\0"
    // 加进去的是【空串】，不是 NUL 字符 —— 结果整个环境块连一个分隔符都没有。
    // 这里改用 vector<char> 一个一个 push_back，才不会踩这个坑。
    std::vector<char> env_block;
    if (!config.env_vars.empty()) {
        for (const auto &env : config.env_vars) {
            const std::string kv = env.first + "=" + env.second;
            env_block.insert(env_block.end(), kv.begin(), kv.end());
            env_block.push_back('\0');
        }
        env_block.push_back('\0');
    }

    const BOOL success = CreateProcessA(
        resolved_path.c_str(),
        const_cast<LPSTR>(command_line.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP,
        env_block.empty() ? nullptr : static_cast<LPVOID>(env_block.data()),
        resolved_working_dir.c_str(),
        &startup_info,
        &proc_info);

    if (!success) {
        log(LogLevel::ERROR, "CreateProcess failed for " + config.name +
                             ", Error: " + std::to_string(GetLastError()));
        return false;
    }

    // Job 对象：把进程塞进去，设成"job 关闭时杀掉所有成员"，
    // 这样守护进程挂了也能连带清掉子进程树。
    HANDLE job = CreateJobObject(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info;
        ZeroMemory(&job_info, sizeof(job_info));
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
            log(LogLevel::WARNING, "Failed to set job object information for " + config.name);
        }
        if (!AssignProcessToJobObject(job, proc_info.hProcess)) {
            log(LogLevel::WARNING, "Failed to assign process to job object for " + config.name);
        }
        // 老代码这里没加锁，跟别的线程的读构成数据竞争
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto existing = process_jobs_.find(config.name);
        if (existing != process_jobs_.end()) {
            // 重启时会走到这儿。不先把旧的关掉就是一个句柄泄漏，
            // 重启一千次就泄漏一千个。
            CloseHandle(existing->second);
        }
        process_jobs_[key] = job;
    } else {
        log(LogLevel::WARNING, "Failed to create job object for " + config.name);
    }

    CloseHandle(proc_info.hThread); // 线程句柄用不上，立刻关
    log(LogLevel::INFO, "Started Windows process: " + config.name +
                        " PID: " + std::to_string(proc_info.dwProcessId));
    return true;
}

/**
 * 停 Windows 进程。CTRL_BREAK_EVENT 相当于 Unix 的 SIGTERM，
 * TerminateProcess 相当于 SIGKILL。
 */
bool Process::terminateWindowsProcess(const std::string &name, PROCESS_INFORMATION &proc_info, int timeout_ms) {
    if (!isWindowsProcessRunning(proc_info)) {
        cleanupWindowsProcess(name, proc_info);
        return true;
    }

    if (timeout_ms > 0) {
        log(LogLevel::INFO, "Attempting graceful shutdown for " + name);
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, proc_info.dwProcessId);
        if (WaitForSingleObject(proc_info.hProcess, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0) {
            log(LogLevel::INFO, "Process " + name + " terminated gracefully");
            cleanupWindowsProcess(name, proc_info);
            return true;
        }
    }

    log(LogLevel::WARNING, "Force terminating process " + name);
    TerminateProcess(proc_info.hProcess, 1);
    WaitForSingleObject(proc_info.hProcess, 5000);
    cleanupWindowsProcess(name, proc_info);
    return true;
}

bool Process::isWindowsProcessRunning(const PROCESS_INFORMATION &proc_info) {
    if (proc_info.hProcess == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    if (GetExitCodeProcess(proc_info.hProcess, &exit_code)) {
        return exit_code == STILL_ACTIVE;
    }
    return false;
}

/**
 * 收拾 Windows 的句柄。进程句柄和 Job 句柄都得关，少一个就泄漏。
 */
void Process::cleanupWindowsProcess(const std::string &name, PROCESS_INFORMATION &proc_info) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (proc_info.hProcess != nullptr) {
        CloseHandle(proc_info.hProcess);
        proc_info.hProcess = nullptr;
    }
    auto job = process_jobs_.find(name);
    if (job != process_jobs_.end()) {
        CloseHandle(job->second);
        process_jobs_.erase(job);
    }
    process_handles_.erase(name);
}

#endif // _WIN32
// ============================ 环境变量 ============================

/**
 * 解析 env_vars 配置项。
 *
 * 格式：逗号分隔的 KEY=VALUE，支持 ${VAR} 引用当前环境变量。
 * 例：env_vars = APP_ENV=prod,LOG_DIR=${HOME}/logs
 *
 * 已知限制：值里不能带逗号。真要带的话目前没辙，只能改格式。
 */
std::map<std::string, std::string> Process::parseEnvVars(const std::string &env_str) {
    std::map<std::string, std::string> env_vars;
    std::istringstream iss(env_str);
    std::string pair;

    while (std::getline(iss, pair, ',')) {
        // 去掉前导空白（"A=1, B=2" 里 B 前面那个空格）
        const size_t begin = pair.find_first_not_of(" \t");
        if (begin == std::string::npos) continue; // 空项，跳过
        pair = pair.substr(begin);

        const size_t pos = pair.find('=');
        if (pos == std::string::npos) {
            log(LogLevel::WARNING, "Ignoring env_vars entry without '=': " + pair);
            continue;
        }

        std::string key = pair.substr(0, pos);
        std::string value = pair.substr(pos + 1);
        // 键名尾部可能带空格，去掉
        const size_t key_end = key.find_last_not_of(" \t");
        key = (key_end == std::string::npos) ? std::string() : key.substr(0, key_end + 1);
        if (key.empty()) {
            log(LogLevel::WARNING, "Ignoring env_vars entry with empty key: " + pair);
            continue;
        }

        // 展开 ${VAR}。注意每替换一次都要把游标往后挪，否则
        // 替换进去的内容如果又含 ${...} 会死循环。
        size_t var_start = 0;
        while ((var_start = value.find("${", var_start)) != std::string::npos) {
            const size_t var_end = value.find('}', var_start);
            if (var_end == std::string::npos) {
                log(LogLevel::WARNING, "Unterminated reference in env var " + key);
                break; // 没有闭合的 }，别再找了，否则原地打转
            }
            const std::string var_name = value.substr(var_start + 2, var_end - var_start - 2);
            const char *var_value = std::getenv(var_name.c_str());
            if (var_value != nullptr) {
                const std::string replacement(var_value);
                value.replace(var_start, var_end - var_start + 1, replacement);
                var_start += replacement.size();
            } else {
                // 引用了不存在的变量：当空串处理，但要告诉用户一声
                log(LogLevel::WARNING, "Environment variable " + var_name + " not defined");
                value.replace(var_start, var_end - var_start + 1, "");
            }
        }

        env_vars[key] = value;
    }

    return env_vars;
}

// ============================ 路径与文件 ============================

/**
 * 相对路径按 working_dir 拼成绝对路径。已经是绝对路径就原样返回。
 *
 * 判断绝对路径这里兼容了三种写法：/xxx（Unix）、\xxx（Windows）、C:xxx（Windows 盘符）。
 */
std::string Process::resolvePath(const std::string &path, const std::string &working_dir) {
    if (path.empty()) return path;

    if (path[0] == '/' || path[0] == '\\' || (path.length() > 1 && path[1] == ':')) {
        return path;
    }

    if (!working_dir.empty()) {
        std::string resolved = working_dir;
        if (resolved.back() != '/' && resolved.back() != '\\') {
            resolved += '/';
        }
        resolved += path;
        return resolved;
    }

    // 既不是绝对路径、又没有工作目录，那就原样返回，让调用方去碰运气
    return path;
}

/**
 * 建目录（只建一级，不递归）。已经存在也算成功。
 *
 * 只建一级是因为这函数只用来兜底 working_dir，用户配多级路径是用户的事。
 */
bool Process::createDirectory(const std::string &path) {
    if (path.empty()) return true;
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

/**
 * 判断是不是一个"普通文件"（不是目录）。
 *
 * 用 S_ISREG 而不是只看 stat 成功，是因为目录也能 stat 成功 ——
 * 配置里把 path 写成目录的话，得在这里拦住。
 */
bool Process::fileExists(const std::string &path) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat buffer;
    return stat(path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode);
#endif
}

// ============================ PID 文件 ============================
//
// 用来防止同一个配置被启动两次。两个实例抢着拉起同一批进程，
// 结果就是每个进程都被拉两份，然后互相抢端口/文件锁，现场极其难看。

/**
 * 拿 pid 文件锁。
 *
 * 用 flock 而不是"读文件里的 pid 再判断进程在不在"，因为后者有 TOCTOU 问题，
 * 而且判断进程在不在本身就是上面说的那个僵尸陷阱。
 * flock 是内核保证的，进程一死锁自动释放，没有这些麻烦。
 */
bool Process::acquirePidFile(const std::string &path) {
#ifndef _WIN32
    const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        log(LogLevel::ERROR, "Cannot open pid file " + path + ": " + strerror(errno));
        return false;
    }
    // LOCK_NB 表示拿不到就立刻返回，不要卡在这儿等
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        log(LogLevel::ERROR, "Another instance already holds " + path + ", refusing to start");
        close(fd);
        return false;
    }
    // 锁到手了，把 pid 写进去（主要是给人看的，程序自己不用它做判断）
    const std::string content = std::to_string(getpid()) + "\n";
    if (ftruncate(fd, 0) != 0 || write(fd, content.c_str(), content.size()) < 0) {
        log(LogLevel::WARNING, "Failed to write pid file " + path);
    }
    // fd 故意不关：一关锁就没了。留到 releasePidFile 再处理。
    pid_file_fd_ = fd;
    log(LogLevel::INFO, "Pid file acquired: " + path);
#endif
    return true;
}

void Process::releasePidFile() {
#ifndef _WIN32
    if (pid_file_fd_ >= 0) {
        flock(pid_file_fd_, LOCK_UN);
        close(pid_file_fd_);
        pid_file_fd_ = -1;
        // 顺手删掉，不然下次启动会看到上次残留的 pid 文件，容易吓人
        if (!pid_file_.empty()) {
            unlink(pid_file_.c_str());
        }
    }
#endif
}

// ============================ 等待原语 ============================

/**
 * 可被打断的 sleep。
 *
 * @return false 表示"总闸被拉了，别等了，赶紧收摊"
 *
 * 为什么不直接用 sleep_for？因为停机时不该等满一个 check_interval
 * 或者一个 60 秒的退避延迟。用条件变量就能被 notify_all 立刻唤醒。
 *
 * 这是全项目唯一一个"睡着等"的地方，其他所有等待都必须走它。
 */
bool Process::waitInterruptible(int64_t ms) {
    if (ms <= 0) {
        return !global_stop_.load();
    }
    std::unique_lock<std::mutex> lock(cv_mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return global_stop_.load(); });
    return !global_stop_.load();
}

// ============================ 日志 ============================

/**
 * 打日志。同时输出到 stdout 和文件（如果配了 log_file）。
 *
 * 三个细节：
 *   1) 换行符要转义。不然用户程序名里带个 \n 就能伪造日志行，把排查方向带偏。
 *   2) 文件流只开一次。以前是每条日志 open/close 一次，日志一多就是灾难。
 *   3) 每次都 flush。守护进程崩溃时最需要日志，缓冲着没刷出去就白记了。
 */
void Process::log(LogLevel level, const std::string &message) {
    if (level < log_level_.load()) {
        return;
    }

    const char *level_str = "INFO";
    switch (level) {
        case LogLevel::DEBUG: level_str = "DEBUG";
            break;
        case LogLevel::INFO: level_str = "INFO";
            break;
        case LogLevel::WARNING: level_str = "WARNING";
            break;
        case LogLevel::ERROR: level_str = "ERROR";
            break;
    }

    // 日志注入防护：把换行替换成空格，一行就是一行
    std::string sanitized;
    sanitized.reserve(message.size());
    for (const char c : message) {
        sanitized += (c == '\n' || c == '\r') ? ' ' : c;
    }

    const std::string log_message = "[" + getCurrentTime() + "] [" + level_str + "] " + sanitized;

    std::lock_guard<std::mutex> lock(log_mutex_);
    std::cout << log_message << std::endl;

    if (log_file_.empty()) {
        return; // 没配日志文件，只走控制台
    }
    if (!log_stream_.is_open()) {
        log_stream_.open(log_file_, std::ios_base::app);
        if (!log_stream_.is_open()) {
            return; // 打不开就算了，别在日志里报日志错误，会递归
        }
    }
    log_stream_ << log_message << std::endl;
    log_stream_.flush();
}

/**
 * 当前时间，格式 "2026-11-03 10:27:20.073"。
 *
 * 用 localtime_r 而不是 localtime：后者返回的是静态缓冲区，多线程同时调
 * 会拿到错乱的时间，甚至崩掉。
 */
std::string Process::getCurrentTime() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &seconds);
#else
    localtime_r(&seconds, &tm_buf);
#endif

    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // 毫秒单独拼，因为 strftime 不支持 %f 这种毫秒占位符
    char out[96];
    std::snprintf(out, sizeof(out), "%s.%03d", buffer, static_cast<int>(ms.count()));
    return std::string(out);
}

// ============================ 参数分割 ============================

/**
 * 把配置里的 args 字符串拆成参数数组。
 *
 * 支持单引号和双引号：args = --name "hello world" 拆成 ["--name", "hello world"]。
 * 引号本身会被吃掉，不传给程序。
 *
 * 注意：这不是 shell，没有变量展开、没有管道重定向、没有通配符。
 * 要那些就自己写个脚本包一层。这是故意的 —— 经过 shell 就有命令注入风险。
 */
std::vector<std::string> Process::splitArgs(const std::string &args) {
    std::vector<std::string> result;
    std::string current;
    bool in_single = false;
    bool in_double = false;

    for (const char c : args) {
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            // 引号外面遇到空白就是参数分隔符
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }

    // 最后一个参数后面没有空白，得单独收一下
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}
