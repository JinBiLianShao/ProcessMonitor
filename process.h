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

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#endif

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

struct ProcessConfig {
    std::string name;
    std::string path;
    std::string working_dir;
    std::string args;
    std::map<std::string, std::string> env_vars;
    int restart_delay;
    int max_restarts;
    int graceful_timeout;
    std::atomic<int> restart_count{0};
    std::atomic<bool> should_stop{false};
    std::atomic<bool> is_running{false};

    // 默认构造函数
    ProcessConfig() = default;

    // 自定义拷贝构造函数
    ProcessConfig(const ProcessConfig& other) :
        name(other.name),
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

    // 自定义赋值运算符
    ProcessConfig& operator=(const ProcessConfig& other) {
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

    // 移动构造函数
    ProcessConfig(ProcessConfig&& other) noexcept :
        name(std::move(other.name)),
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

    // 移动赋值运算符
    ProcessConfig& operator=(ProcessConfig&& other) noexcept {
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

class Process {
public:
    Process();
    ~Process();

    bool loadConfig(const std::string& config_file);
    bool startAll();
    bool stopAll();
    void waitAll();
    void setLogLevel(LogLevel level);

private:
    std::map<std::string, ProcessConfig> processes_;
    std::map<std::string, std::thread> monitor_threads_;
    std::map<std::string, std::thread> zombie_cleaner_threads_;
    std::atomic<bool> global_stop_{false};
    LogLevel log_level_{LogLevel::INFO};
    std::string log_file_;
    int check_interval_{2};

    mutable std::mutex log_mutex_;
    mutable std::mutex process_mutex_;

#ifdef _WIN32
    std::map<std::string, PROCESS_INFORMATION> process_handles_;
    std::map<std::string, HANDLE> process_jobs_;  // 用于进程组管理
#else
    std::map<std::string, pid_t> process_pids_;
    std::map<std::string, pid_t> process_groups_; // 用于进程组管理
#endif

    void monitorProcess(const std::string& process_name);
    void zombieCleaner(const std::string& process_name);
    bool startProcess(ProcessConfig& config);
    bool stopProcess(const std::string& process_name, bool force = false);
    bool isProcessRunning(const std::string& process_name);
    bool validateProcessConfig(const ProcessConfig& config);
    std::string resolvePath(const std::string& path, const std::string& working_dir);

    // 平台相关实现
#ifdef _WIN32
    bool createWindowsProcess(ProcessConfig& config, PROCESS_INFORMATION& proc_info);
    bool terminateWindowsProcess(PROCESS_INFORMATION& proc_info, int timeout_ms);
    bool isWindowsProcessRunning(PROCESS_INFORMATION& proc_info);
    void cleanupWindowsProcess(PROCESS_INFORMATION& proc_info);
#else
    bool createUnixProcess(ProcessConfig& config, pid_t& pid);
    bool terminateUnixProcess(pid_t pid, pid_t process_group, int timeout_sec);
    bool isUnixProcessRunning(pid_t pid);
    void cleanupUnixProcess(pid_t pid);
    bool setupProcessGroup(pid_t pid, pid_t& process_group);
#endif

    // 环境变量处理
    std::map<std::string, std::string> parseEnvVars(const std::string& env_str);
    void setupEnvironment(const std::map<std::string, std::string>& env_vars);

    // 日志系统
    void log(LogLevel level, const std::string& message);
    void logToFile(const std::string& message);

    // 工具函数
    std::vector<std::string> splitArgs(const std::string& args);
    std::string getCurrentTime();
    bool createDirectory(const std::string& path);
    bool fileExists(const std::string& path);
};

#endif // PROCESS_DAEMON_H
