/**
 * @file process.h
 * @brief 进程管理守护进程头文件
 * @author jinbilianshao
 *
 * 定义了进程管理守护进程的核心类和数据结构，用于监控和管理多个子进程。
 * 支持跨平台（Windows/Linux）进程管理，提供配置加载、进程启动/停止、
 * 状态监控、僵尸进程清理等功能。
 */

#ifndef PROCESS_DAEMON_H
#define PROCESS_DAEMON_H

#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <fstream>

// 根据不同平台包含相应的系统头文件
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#endif

/**
 * @enum LogLevel
 * @brief 日志级别枚举
 *
 * 定义了不同的日志级别，用于控制日志输出的详细程度
 */
enum class LogLevel {
    DEBUG, ///< 调试信息
    INFO, ///< 普通信息
    WARNING, ///< 警告信息
    ERROR ///< 错误信息
};

/**
 * @struct ProcessConfig
 * @brief 进程配置结构体
 *
 * 存储单个进程的配置信息，包括路径、参数、环境变量等
 */
struct ProcessConfig {
    std::string name; ///< 进程名称
    std::string path; ///< 可执行文件路径
    std::string working_dir; ///< 工作目录
    std::string args; ///< 命令行参数
    std::map<std::string, std::string> env_vars; ///< 环境变量
    int restart_delay; ///< 重启延迟时间（秒）
    int max_restarts; ///< 最大重启次数（-1表示无限重启）
    int graceful_timeout; ///< 优雅关闭超时时间（秒）
    std::atomic<int> restart_count{0}; ///< 重启计数器
    std::atomic<bool> should_stop{false}; ///< 停止标志
    std::atomic<bool> is_running{false}; ///< 运行状态标志

    /**
     * @brief 默认构造函数
     */
    ProcessConfig() = default;

    /**
     * @brief 拷贝构造函数
     *
     * 由于包含std::atomic成员，需要自定义拷贝构造函数
     * @param other 要拷贝的对象
     */
    ProcessConfig(const ProcessConfig &other) : name(other.name),
                                                path(other.path),
                                                working_dir(other.working_dir),
                                                args(other.args),
                                                env_vars(other.env_vars),
                                                restart_delay(other.restart_delay),
                                                max_restarts(other.max_restarts),
                                                graceful_timeout(other.graceful_timeout) {
        // atomic变量需要单独处理
        restart_count.store(other.restart_count.load());
        should_stop.store(other.should_stop.load());
        is_running.store(other.is_running.load());
    }

    /**
     * @brief 赋值运算符重载
     *
     * 由于包含std::atomic成员，需要自定义赋值运算符
     * @param other 要赋值的对象
     * @return ProcessConfig& 当前对象的引用
     */
    ProcessConfig &operator=(const ProcessConfig &other) {
        if (this != &other) {
            name = other.name;
            path = other.path;
            working_dir = other.working_dir;
            args = other.args;
            env_vars = other.env_vars;
            restart_delay = other.restart_delay;
            max_restarts = other.max_restarts;
            graceful_timeout = other.graceful_timeout;

            // atomic变量需要单独处理
            restart_count.store(other.restart_count.load());
            should_stop.store(other.should_stop.load());
            is_running.store(other.is_running.load());
        }
        return *this;
    }

    /**
     * @brief 移动构造函数
     *
     * 由于包含std::atomic成员，需要自定义移动构造函数
     * @param other 要移动的对象
     */
    ProcessConfig(ProcessConfig &&other) noexcept : name(std::move(other.name)),
                                                    path(std::move(other.path)),
                                                    working_dir(std::move(other.working_dir)),
                                                    args(std::move(other.args)),
                                                    env_vars(std::move(other.env_vars)),
                                                    restart_delay(other.restart_delay),
                                                    max_restarts(other.max_restarts),
                                                    graceful_timeout(other.graceful_timeout) {
        // atomic变量需要单独处理
        restart_count.store(other.restart_count.load());
        should_stop.store(other.should_stop.load());
        is_running.store(other.is_running.load());
    }

    /**
     * @brief 移动赋值运算符重载
     *
     * 由于包含std::atomic成员，需要自定义移动赋值运算符
     * @param other 要移动赋值的对象
     * @return ProcessConfig& 当前对象的引用
     */
    ProcessConfig &operator=(ProcessConfig &&other) noexcept {
        if (this != &other) {
            name = std::move(other.name);
            path = std::move(other.path);
            working_dir = std::move(other.working_dir);
            args = std::move(other.args);
            env_vars = std::move(other.env_vars);
            restart_delay = other.restart_delay;
            max_restarts = other.max_restarts;
            graceful_timeout = other.graceful_timeout;

            // atomic变量需要单独处理
            restart_count.store(other.restart_count.load());
            should_stop.store(other.should_stop.load());
            is_running.store(other.is_running.load());
        }
        return *this;
    }
};

/**
 * @class Process
 * @brief 进程管理守护类
 *
 * 负责加载配置、管理进程生命周期、监控进程状态、清理僵尸进程等核心功能
 */
class Process {
public:
    /**
     * @brief 构造函数
     */
    Process();

    /**
     * @brief 析构函数
     */
    ~Process();

    /**
     * @brief 加载配置文件
     *
     * 从指定的配置文件中读取并解析进程配置信息
     * @param config_file 配置文件路径
     * @return true 配置加载成功
     * @return false 配置加载失败
     */
    bool loadConfig(const std::string &config_file);

    /**
     * @brief 启动所有配置的进程
     *
     * 为每个配置的进程创建监控线程和僵尸清理线程
     * @return true 启动成功
     * @return false 启动失败
     */
    bool startAll();

    /**
     * @brief 停止所有进程
     *
     * 设置全局停止标志并等待所有线程结束
     * @return true 停止成功
     * @return false 停止失败
     */
    bool stopAll();

    /**
     * @brief 等待所有监控线程结束
     *
     * 阻塞等待所有监控线程执行完毕
     */
    void waitAll();

    /**
     * @brief 设置日志级别
     *
     * @param level 日志级别
     */
    void setLogLevel(LogLevel level);

private:
    std::map<std::string, ProcessConfig> processes_; ///< 进程配置映射
    std::map<std::string, std::thread> monitor_threads_; ///< 监控线程映射
    std::map<std::string, std::thread> zombie_cleaner_threads_; ///< 僵尸清理线程映射
    std::atomic<bool> global_stop_{false}; ///< 全局停止标志
    LogLevel log_level_{LogLevel::INFO}; ///< 日志级别
    std::string log_file_; ///< 日志文件路径
    int check_interval_{2}; ///< 进程检查间隔（秒）

    mutable std::mutex log_mutex_; ///< 日志互斥锁
    mutable std::mutex process_mutex_; ///< 进程互斥锁

    // Windows平台特有成员
#ifdef _WIN32
    std::map<std::string, PROCESS_INFORMATION> process_handles_; ///< Windows进程句柄映射
    std::map<std::string, HANDLE> process_jobs_; ///< Windows作业对象映射（用于进程组管理）
#else
    // Unix/Linux平台特有成员
    std::map<std::string, pid_t> process_pids_; ///< Unix进程ID映射
    std::map<std::string, pid_t> process_groups_; ///< Unix进程组映射
#endif

    /**
     * @brief 监控进程状态
     *
     * 持续检查指定进程的运行状态，如果进程停止则根据配置决定是否重启
     * @param process_name 要监控的进程名称
     */
    void monitorProcess(const std::string &process_name);

    /**
     * @brief 清理僵尸进程
     *
     * 定期检查并清理已终止但仍占用系统资源的子进程
     * @param process_name 进程名称
     */
    void zombieCleaner(const std::string &process_name);

    /**
     * @brief 启动指定进程
     *
     * 根据配置信息启动一个新的子进程
     * @param config 进程配置信息
     * @return true 启动成功
     * @return false 启动失败
     */
    bool startProcess(ProcessConfig &config);

    /**
     * @brief 停止指定进程
     *
     * 根据进程名称停止对应的子进程
     * @param process_name 进程名称
     * @param force 是否强制停止
     * @return true 停止成功
     * @return false 停止失败
     */
    bool stopProcess(const std::string &process_name, bool force = false);

    /**
     * @brief 检查进程是否正在运行
     *
     * @param process_name 进程名称
     * @return true 进程正在运行
     * @return false 进程未运行
     */
    bool isProcessRunning(const std::string &process_name);

    /**
     * @brief 验证进程配置
     *
     * 检查进程配置是否有效
     * @param config 进程配置
     * @return true 配置有效
     * @return false 配置无效
     */
    bool validateProcessConfig(const ProcessConfig &config);

    /**
     * @brief 解析路径
     *
     * 根据工作目录解析相对路径为绝对路径
     * @param path 路径
     * @param working_dir 工作目录
     * @return std::string 解析后的路径
     */
    std::string resolvePath(const std::string &path, const std::string &working_dir);

    // 平台相关实现
#ifdef _WIN32
    /**
     * @brief 创建Windows进程
     *
     * 在Windows平台上创建一个新的进程
     * @param config 进程配置
     * @param proc_info 进程信息结构体
     * @return true 创建成功
     * @return false 创建失败
     */
    bool createWindowsProcess(ProcessConfig &config, PROCESS_INFORMATION &proc_info);

    /**
     * @brief 终止Windows进程
     *
     * @param proc_info 进程信息
     * @param timeout_ms 超时时间（毫秒）
     * @return true 终止成功
     * @return false 终止失败
     */
    bool terminateWindowsProcess(PROCESS_INFORMATION &proc_info, int timeout_ms);

    /**
     * @brief 检查Windows进程是否正在运行
     *
     * @param proc_info 进程信息
     * @return true 进程正在运行
     * @return false 进程未运行
     */
    bool isWindowsProcessRunning(PROCESS_INFORMATION &proc_info);

    /**
     * @brief 清理Windows进程资源
     *
     * @param proc_info 进程信息
     */
    void cleanupWindowsProcess(PROCESS_INFORMATION &proc_info);
#else
    /**
     * @brief 创建Unix进程
     *
     * 在Unix/Linux平台上通过fork创建一个新的进程
     * @param config 进程配置
     * @param pid 进程ID（输出参数）
     * @return true 创建成功
     * @return false 创建失败
     */
    bool createUnixProcess(ProcessConfig &config, pid_t &pid);

    /**
     * @brief 终止Unix进程
     *
     * @param pid 进程ID
     * @param process_group 进程组ID
     * @param timeout_sec 超时时间（秒）
     * @return true 终止成功
     * @return false 终止失败
     */
    bool terminateUnixProcess(pid_t pid, pid_t process_group, int timeout_sec);

    /**
     * @brief 检查Unix进程是否正在运行
     *
     * @param pid 进程ID
     * @return true 进程正在运行
     * @return false 进程未运行
     */
    bool isUnixProcessRunning(pid_t pid);

    /**
     * @brief 清理Unix进程资源
     *
     * @param pid 进程ID
     */
    void cleanupUnixProcess(pid_t pid);

    /**
     * @brief 设置进程组
     *
     * @param pid 进程ID
     * @param process_group 进程组ID（输出参数）
     * @return true 设置成功
     * @return false 设置失败
     */
    bool setupProcessGroup(pid_t pid, pid_t &process_group);
#endif

    // 环境变量处理
    /**
     * @brief 解析环境变量字符串
     *
     * 将逗号分隔的环境变量字符串解析为键值对映射
     * @param env_str 环境变量字符串
     * @return std::map<std::string, std::string> 解析后的环境变量映射
     */
    std::map<std::string, std::string> parseEnvVars(const std::string &env_str);

    /**
     * @brief 设置环境变量
     *
     * 根据配置设置子进程的环境变量
     * @param env_vars 环境变量映射
     */
    void setupEnvironment(const std::map<std::string, std::string> &env_vars);

    // 日志系统
    /**
     * @brief 记录日志
     *
     * 根据日志级别记录日志信息到控制台和文件
     * @param level 日志级别
     * @param message 日志消息
     */
    void log(LogLevel level, const std::string &message);

    /**
     * @brief 将日志写入文件
     *
     * @param message 日志消息
     */
    void logToFile(const std::string &message);

    // 工具函数
    /**
     * @brief 分割命令行参数
     *
     * 将命令行参数字符串分割为参数列表
     * @param args 参数字符串
     * @return std::vector<std::string> 参数列表
     */
    std::vector<std::string> splitArgs(const std::string &args);

    /**
     * @brief 获取当前时间字符串
     *
     * @return std::string 格式化的时间字符串
     */
    std::string getCurrentTime();

    /**
     * @brief 创建目录
     *
     * 创建指定路径的目录（如果不存在）
     * @param path 目录路径
     * @return true 创建成功或目录已存在
     * @return false 创建失败
     */
    bool createDirectory(const std::string &path);

    /**
     * @brief 检查文件是否存在
     *
     * @param path 文件路径
     * @return true 文件存在
     * @return false 文件不存在
     */
    bool fileExists(const std::string &path);
};

#endif // PROCESS_DAEMON_H
