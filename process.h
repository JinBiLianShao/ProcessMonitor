/**
 * @file process.h
 * @brief ProcessMonitor 的头文件。一个配置结构 + 一个守护类，就这些。
 * @author jinbilianshao
 *
 * 几个设计上的取舍先说清楚，免得后来人改的时候踩坑：
 *
 * 1) 配置和运行态拆成两个 struct。
 *    以前揉在一起，因为带了 atomic，编译器就不给生成拷贝构造了，只能手写四份
 *    （拷贝构造、拷贝赋值、移动构造、移动赋值），每份都得把所有字段抄一遍 ——
 *    以后加个字段忘了同步，就是那种查一下午的 bug。现在 ProcessConfig 是纯 POD，
 *    拷贝移动全交给编译器；只有真正会变的东西才扔进 ProcessRuntime。
 *
 * 2) 每个被托管的进程有自己的监控线程，但回收子进程的只有【一个】线程。
 *    原因很土：一个子进程只能被 waitpid 一次，多线程抢着 wait 迟早出事。
 *    所以 pid -> 进程名 这张表由 reaper 独占维护，谁想知道"还活着吗"就去查表，
 *    别自己 kill(pid, 0) 瞎猜 —— 僵尸进程对 kill(pid,0) 照样返回 0。
 *
 * 3) 跨线程共享的字段一律 atomic。能不加锁就别加锁，锁多了就是死锁温床：
 *    这项目以前就在 fork 之后的子进程里拿着父进程的锁去打日志，直接卡死。
 */

#ifndef PROCESS_DAEMON_H
#define PROCESS_DAEMON_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// windows.h 里有 #define ERROR 0，会把下面的 LogLevel::ERROR 冲成
// LogLevel::0，编 Windows 版时报一片 "expected identifier before numeric constant"。
// 我们不用这个宏，直接撤掉，枚举名就能跨平台保持一致。
#undef ERROR
#else
#include <sys/types.h>
#endif

/**
 * @enum LogLevel
 * @brief 日志级别。比较用的是枚举值大小，所以顺序别乱动。
 */
enum class LogLevel {
    DEBUG,   ///< 排查问题用，平时别开，日志量大得吓人
    INFO,    ///< 默认级别。启动、停止、重启这些事都在这
    WARNING, ///< 进程挂了、要重启了、配置项写歪了
    ERROR    ///< 起不来、exec 失败、预算耗尽
};

/**
 * @struct ProcessConfig
 * @brief 单个进程的配置。loadConfig 之后就是只读的，谁都不许改。
 *
 * 注意所有数值字段都带默认值。这不是多此一举 —— 以前没有默认值，
 * 配置文件里漏写一行 max_restarts，读到的就是栈上的垃圾数据，
 * 结果进程一次都没起来就被判定"重启次数超限"，排查了半天。
 * 现在漏写就用默认值，写错了就打 WARNING，绝不静默。
 */
struct ProcessConfig {
    std::string name;                            ///< 显示名。默认等于配置节名，可以用 name 键改
    std::string path;                            ///< 可执行文件路径。相对路径按 working_dir 解析
    std::string working_dir;                     ///< 工作目录。留空就取 path 的父目录
    std::string args;                            ///< 命令行参数。支持单双引号
    std::map<std::string, std::string> env_vars; ///< 追加/覆盖的环境变量，会先继承父进程的
    int restart_delay = 3;                       ///< 第一次重启前等几秒，之后按 2 的 n 次方翻倍
    int restart_backoff_max = 60;                ///< 退避的上限，翻到这儿就不再涨了
    int max_restarts = 5;                        ///< 最多重启几次。-1 是无限，0 是"只启动不重启"
    int graceful_timeout = 10;                   ///< 发完 SIGTERM 等几秒，还不走就 SIGKILL
    int restart_reset_after = 60;                ///< 稳定跑够这么多秒就把重启计数清零，0 表示不清
};

/**
 * @struct ProcessRuntime
 * @brief 单个进程的运行态。全是 atomic，因为监控线程和 reaper 线程都要读写。
 *
 * 为什么单独拎出来？见文件头第 1 条。简单说：混在 ProcessConfig 里就没法
 * 让编译器生成拷贝构造了。
 */
struct ProcessRuntime {
    std::atomic<int> restart_count{0};     ///< 已经用掉的重启次数
    std::atomic<bool> should_stop{false};  ///< 要停这个进程了，监控线程看到就退出
    std::atomic<bool> is_running{false};   ///< 最后一次观测到的状态。必须在 state_mutex_ 里翻
    std::atomic<bool> gave_up{false};      ///< 预算烧完了，彻底放弃
    std::atomic<bool> started_once{false}; ///< 成功启动过没有。首次启动不占重启预算
    std::atomic<int64_t> started_at_ms{0}; ///< 上次启动成功的时刻（epoch 毫秒），用来算 uptime
    std::atomic<int> last_exit_code{-1};   ///< 上次退出码，正常退出才有值
    std::atomic<int> last_term_signal{0};  ///< 上次被哪个信号干掉的，被信号杀时才有值
};

/**
 * @class Process
 * @brief 守护类本体。
 *
 * 生命周期就三步：loadConfig -> startAll -> (等信号) -> stopAll。
 * 别在 startAll 之后再改配置，也别在多线程里 new 这个类。
 */
class Process {
public:
    Process();
    ~Process();

    // 这玩意儿持有线程和文件描述符，拷贝一份没有意义，直接禁掉
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    /**
     * @brief 读配置文件。
     * @param config_file 路径
     * @return true 表示至少读到一个能用的进程配置；全是废的就返回 false
     *
     * 未知的键会打 WARNING 而不是静默忽略 —— 拼错一个字母查半天，太亏。
     */
    bool loadConfig(const std::string &config_file);

    /**
     * @brief 起线程：先起 reaper，再给每个进程起监控线程。
     * @return false 表示没配置可跑，或者 pid 文件被别人占了
     */
    bool startAll();

    /**
     * @brief 停一切。可以重复调，第二次开始直接返回。
     *
     * 顺序很重要：先让监控线程退出（否则它还会往上拉进程），
     * 再挨个优雅停止，最后等 reaper 收尸。
     */
    bool stopAll();

    /**
     * @brief 手动改日志级别。目前没人调，留着给以后做热重载用。
     */
    void setLogLevel(LogLevel level);

private:
    // ---- 数据 ----
    std::map<std::string, ProcessConfig> processes_;  ///< 配置。键是【配置节名】，不是显示名！
    std::map<std::string, ProcessRuntime> runtimes_;  ///< 运行态，键同上
    std::map<std::string, std::thread> monitor_threads_;
    std::thread reaper_thread_;

    std::atomic<bool> global_stop_{false};            ///< 全体停机的总闸
    std::atomic<bool> stopping_{false};               ///< 防止 stopAll 重入
    std::atomic<LogLevel> log_level_{LogLevel::INFO};

    std::string log_file_;
    std::ofstream log_stream_;                        ///< 只开一次，别每条日志都 open/close
    int check_interval_{2};                           ///< 监控线程多久看一次（秒）

    mutable std::mutex log_mutex_;                    ///< 护着 cout 和 log_stream_
    mutable std::mutex state_mutex_;                  ///< 护着下面那几张 pid/handle 表
    std::mutex cv_mutex_;                             ///< 配 condition_variable 用的
    std::condition_variable cv_;                      ///< 拿它做可中断的 sleep

    std::string pid_file_;
    int pid_file_fd_{-1};                             ///< 留着 fd 是为了退出时能 flock(LOCK_UN)

#ifdef _WIN32
    std::map<std::string, PROCESS_INFORMATION> process_handles_;
    std::map<std::string, HANDLE> process_jobs_;      ///< Job 对象，用来一锅端掉整个进程树
#else
    std::map<std::string, pid_t> process_pids_;       ///< 键是配置节名。这张表就是"权威真相"
    std::map<std::string, pid_t> process_groups_;     ///< 进程组号，kill 的时候要用负数
#endif

    // ---- 线程主体 ----
    void monitorProcess(const std::string &process_name);
    void reaperLoop();
    void handleChildExit(pid_t pid, int status);
    void reapAllChildren();

    // ---- 进程控制 ----
    // key 是配置节名，config 是那份配置。两个都传是因为显示名可能和节名不一样，
    // 存表用 key、打日志用 config.name，别搞混。
    bool startProcess(const std::string &key, const ProcessConfig &config);
    bool stopProcess(const std::string &process_name, bool force);
    bool validateProcessConfig(const ProcessConfig &config);
    std::string resolvePath(const std::string &path, const std::string &working_dir);

#ifdef _WIN32
    bool createWindowsProcess(const std::string &key, const ProcessConfig &config, PROCESS_INFORMATION &proc_info);
    bool terminateWindowsProcess(const std::string &name, PROCESS_INFORMATION &proc_info, int timeout_ms);
    bool isWindowsProcessRunning(const PROCESS_INFORMATION &proc_info);
    void cleanupWindowsProcess(const std::string &name, PROCESS_INFORMATION &proc_info);
#else
    bool createUnixProcess(const ProcessConfig &config, pid_t &pid);
    bool terminateUnixProcess(pid_t pid, pid_t process_group, int timeout_sec);
    bool isUnixProcessRunning(pid_t pid);
#endif

    // ---- 配置解析 ----
    std::map<std::string, std::string> parseEnvVars(const std::string &env_str);

    // 所有数值型配置项都走这个。返回值表示"解析成功没有"，
    // 失败时 out 保持原样（也就是默认值），外面不用管。
    bool parseIntSetting(const std::string &section, const std::string &key,
                         const std::string &raw, int min_value, int max_value, int &out);

    // ---- 日志 ----
    void log(LogLevel level, const std::string &message);

    // ---- 杂活 ----
    std::vector<std::string> splitArgs(const std::string &args);
    std::string getCurrentTime();
    bool createDirectory(const std::string &path);
    bool fileExists(const std::string &path);

    // 可被 stopAll 打断的 sleep。返回 false 意思是"别等了，赶紧收摊"。
    bool waitInterruptible(int64_t ms);

    bool acquirePidFile(const std::string &path);
    void releasePidFile();
};

#endif // PROCESS_DAEMON_H
