/**
 * @file process.cpp
 * @brief 进程管理守护进程实现文件
 * @author jinbilianshao
 *
 * 该文件实现了 Process 类，用于监控和管理多个子进程。
 * 提供配置加载、进程启动/停止、状态监控、僵尸进程清理等功能。
 */

#include "process.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <errno.h>

/**
 * @brief Process 类构造函数
 *
 * 初始化进程守护程序，记录初始化日志信息
 */
Process::Process() {
    log(LogLevel::INFO, "Process Daemon Initialized");
}

/**
 * @brief Process 类析构函数
 *
 * 在对象销毁时停止所有子进程并清理相关资源
 */
Process::~Process() {
    stopAll();

#ifdef _WIN32
    // 清理Windows资源
    for (auto &pair: process_jobs_) {
        if (pair.second != INVALID_HANDLE_VALUE) {
            CloseHandle(pair.second);
        }
    }
    process_jobs_.clear();
#endif
}

/**
 * @brief 加载配置文件
 *
 * 从指定的配置文件中读取并解析进程配置信息
 *
 * @param config_file 配置文件路径
 * @return true 配置加载成功
 * @return false 配置加载失败
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

    // 逐行读取配置文件
    while (std::getline(file, line)) {
        // 移除前后空白字符
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // 跳过空行和注释
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        // 检查节(section)
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            current_section = line.substr(1, line.length() - 2);
            if (current_section != "Global") {
                // 使用就地构造避免拷贝问题
                processes_.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(current_section),
                                   std::forward_as_tuple());
                processes_[current_section].name = current_section;
            }

            continue;
        }

        // 解析键值对
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // 移除键值对的空白字符
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (current_section == "Global") {
                // 处理全局配置项
                if (key == "log_level") {
                    if (value == "DEBUG") log_level_ = LogLevel::DEBUG;
                    else if (value == "INFO") log_level_ = LogLevel::INFO;
                    else if (value == "WARNING") log_level_ = LogLevel::WARNING;
                    else if (value == "ERROR") log_level_ = LogLevel::ERROR;
                } else if (key == "log_file") {
                    log_file_ = value;
                } else if (key == "check_interval") {
                    try {
                        check_interval_ = std::stoi(value);
                    } catch (const std::exception &e) {
                        log(LogLevel::WARNING, "Invalid check_interval value: " + value + ", using default");
                    }
                }
            } else {
                // 处理进程配置项
                auto &config = processes_[current_section];

                if (key == "path") {
                    config.path = value;
                } else if (key == "working_dir") {
                    config.working_dir = value;
                } else if (key == "args") {
                    config.args = value;
                } else if (key == "env_vars") {
                    config.env_vars = parseEnvVars(value);
                } else if (key == "restart_delay") {
                    try {
                        config.restart_delay = std::stoi(value);
                    } catch (const std::exception &e) {
                        log(LogLevel::WARNING, "Invalid restart_delay value for " + current_section + ": " + value);
                    }
                } else if (key == "max_restarts") {
                    try {
                        config.max_restarts = std::stoi(value);
                    } catch (const std::exception &e) {
                        log(LogLevel::WARNING, "Invalid max_restarts value for " + current_section + ": " + value);
                    }
                } else if (key == "graceful_timeout") {
                    try {
                        config.graceful_timeout = std::stoi(value);
                    } catch (const std::exception &e) {
                        log(LogLevel::WARNING, "Invalid graceful_timeout value for " + current_section + ": " + value);
                    }
                }
            }
        }
    }

    // 验证配置
    for (auto it = processes_.begin(); it != processes_.end();) {
        if (!validateProcessConfig(it->second)) {
            log(LogLevel::ERROR, "Invalid configuration for process: " + it->first);
            it = processes_.erase(it);
        } else {
            ++it;
        }
    }

    log(LogLevel::INFO, "Loaded " + std::to_string(processes_.size()) + " process configurations");
    return true;
}

/**
 * @brief 启动所有配置的进程
 *
 * 为每个配置的进程创建监控线程和僵尸清理线程
 *
 * @return true 启动成功
 * @return false 启动失败
 */
bool Process::startAll() {
    global_stop_ = false;

    for (auto &pair: processes_) {
        const std::string &name = pair.first;
        ProcessConfig &config = pair.second;

        // 初始化进程状态
        config.should_stop = false;
        config.restart_count = 0;
        config.is_running = false;

        // 启动监控线程
        monitor_threads_[name] = std::thread(&Process::monitorProcess, this, name);

        // 启动僵尸清理线程
        zombie_cleaner_threads_[name] = std::thread(&Process::zombieCleaner, this, name);

        log(LogLevel::INFO, "Started monitor and cleaner for: " + name);
    }

    return true;
}

/**
 * @brief 停止所有进程
 *
 * 设置全局停止标志并等待所有线程结束
 *
 * @return true 停止成功
 * @return false 停止失败
 */
bool Process::stopAll() {
    global_stop_ = true;

    // 停止所有进程
    for (auto &pair: processes_) {
        pair.second.should_stop = true;
        stopProcess(pair.first, true); // 强制停止
    }

    // 等待所有监控线程结束
    for (auto &pair: monitor_threads_) {
        if (pair.second.joinable()) {
            pair.second.join();
        }
    }

    // 等待所有清理线程结束
    for (auto &pair: zombie_cleaner_threads_) {
        if (pair.second.joinable()) {
            pair.second.join();
        }
    }

    monitor_threads_.clear();
    zombie_cleaner_threads_.clear();

    log(LogLevel::INFO, "All processes stopped");
    return true;
}

/**
 * @brief 等待所有监控线程结束
 *
 * 阻塞等待所有监控线程执行完毕
 */
void Process::waitAll() {
    for (auto &pair: monitor_threads_) {
        if (pair.second.joinable()) {
            pair.second.join();
        }
    }
}

/**
 * @brief 监控进程状态
 *
 * 持续检查指定进程的运行状态，如果进程停止则根据配置决定是否重启
 *
 * @param process_name 要监控的进程名称
 */
void Process::monitorProcess(const std::string &process_name) {
    while (!global_stop_) {
        // 获取配置时加锁保护
        std::unique_lock<std::mutex> lock(process_mutex_);
        auto it = processes_.find(process_name);
        if (it == processes_.end()) {
            lock.unlock();
            break;
        }

        ProcessConfig &config = it->second;
        bool should_stop = config.should_stop;
        bool is_running = config.is_running;
        lock.unlock();

        if (should_stop) {
            break;
        }

        // 检查进程状态时不加锁
        bool running = isProcessRunning(process_name);

        if (!running && !is_running) {
            // 再次获取配置并检查重启次数
            lock.lock();
            int restart_count = config.restart_count;
            int max_restarts = config.max_restarts;
            lock.unlock();

            if (restart_count < max_restarts || max_restarts == -1) {
                log(LogLevel::WARNING,
                    "Process " + process_name + " is not running. Restarting... (" +
                    std::to_string(restart_count + 1) + "/" +
                    (max_restarts == -1 ? "∞" : std::to_string(max_restarts)) + ")");

                if (startProcess(config)) {
                    // 更新重启计数
                    lock.lock();
                    config.restart_count++;
                    config.is_running = true;
                    lock.unlock();
                } else {
                    log(LogLevel::ERROR, "Failed to restart process: " + process_name);
                }
            } else {
                log(LogLevel::ERROR, "Max restarts reached for: " + process_name + ". Giving up.");
                break;
            }
        }

        // 等待检查间隔
        for (int i = 0; i < check_interval_ * 10 && !global_stop_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // 再次检查是否应该停止
            lock.lock();
            bool should_stop_inner = config.should_stop;
            lock.unlock();

            if (should_stop_inner) {
                break;
            }
        }
    }

    log(LogLevel::INFO, "Monitor thread stopped for: " + process_name);
}

/**
 * @brief 清理僵尸进程
 *
 * 定期检查并清理已终止但仍占用系统资源的子进程
 *
 * @param process_name 进程名称
 */
void Process::zombieCleaner(const std::string &process_name) {
    while (!global_stop_) {
        // 获取配置时加锁保护
        std::unique_lock<std::mutex> lock(process_mutex_);
        auto it = processes_.find(process_name);
        if (it == processes_.end()) {
            lock.unlock();
            break;
        }

        ProcessConfig &config = it->second;
        bool should_stop = config.should_stop;
        lock.unlock();

        if (should_stop) {
            break;
        }

#ifdef _WIN32
        // Windows 平台清理
        lock.lock();
        auto handle_it = process_handles_.find(process_name);
        lock.unlock();

        if (handle_it != process_handles_.end()) {
            DWORD exit_code;
            if (GetExitCodeProcess(handle_it->second.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                log(LogLevel::DEBUG, "Cleaning up terminated Windows process: " + process_name);
                cleanupWindowsProcess(handle_it->second);

                lock.lock();
                process_handles_.erase(handle_it);
                config.is_running = false;
                lock.unlock();
            }
        }
#else
        // Unix 平台僵尸进程清理
        lock.lock();
        auto pid_it = process_pids_.find(process_name);
        lock.unlock();

        if (pid_it != process_pids_.end()) {
            int status;
            pid_t result = waitpid(pid_it->second, &status, WNOHANG);
            if (result > 0) {
                log(LogLevel::DEBUG, "Cleaned up terminated Unix process: " + process_name +
                                     " PID: " + std::to_string(pid_it->second));
                cleanupUnixProcess(pid_it->second);

                lock.lock();
                process_pids_.erase(pid_it);
                config.is_running = false;
                lock.unlock();
            } else if (result == -1 && errno == ECHILD) {
                // 进程已经不存在
                lock.lock();
                process_pids_.erase(pid_it);
                config.is_running = false;
                lock.unlock();
            }
        }
#endif

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    log(LogLevel::INFO, "Zombie cleaner thread stopped for: " + process_name);
}

/**
 * @brief 启动指定进程
 *
 * 根据配置信息启动一个新的子进程
 *
 * @param config 进程配置信息
 * @return true 启动成功
 * @return false 启动失败
 */
bool Process::startProcess(ProcessConfig &config) {
    // 解析工作目录和路径
    std::string resolved_path = resolvePath(config.path, config.working_dir);
    std::string resolved_working_dir = config.working_dir.empty()
                                           ? config.path.substr(0, config.path.find_last_of("/\\"))
                                           : config.working_dir;

    if (!fileExists(resolved_path)) {
        log(LogLevel::ERROR, "Executable not found: " + resolved_path);
        return false;
    }

    // 创建工作目录（如果不存在）
    if (!createDirectory(resolved_working_dir)) {
        log(LogLevel::WARNING, "Cannot create working directory: " + resolved_working_dir);
    }

#ifdef _WIN32
    PROCESS_INFORMATION proc_info;
    if (createWindowsProcess(config, proc_info)) {
        std::lock_guard<std::mutex> lock(process_mutex_);
        process_handles_[config.name] = proc_info;
        return true;
    }
#else
    pid_t pid;
    pid_t process_group;
    if (createUnixProcess(config, pid)) {
        std::lock_guard<std::mutex> lock(process_mutex_);
        process_pids_[config.name] = pid;
        if (setupProcessGroup(pid, process_group)) {
            process_groups_[config.name] = process_group;
        }
        return true;
    }
#endif
    return false;
}

/**
 * @brief 停止指定进程
 *
 * 根据进程名称停止对应的子进程
 *
 * @param process_name 进程名称
 * @param force 是否强制停止
 * @return true 停止成功
 * @return false 停止失败
 */
bool Process::stopProcess(const std::string &process_name, bool force) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it = process_handles_.find(process_name);
    if (it != process_handles_.end()) {
        int timeout = force ? 0 : processes_[process_name].graceful_timeout * 1000;
        return terminateWindowsProcess(it->second, timeout);
    }
#else
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it_pid = process_pids_.find(process_name);
    auto it_group = process_groups_.find(process_name);
    if (it_pid != process_pids_.end()) {
        pid_t group = (it_group != process_groups_.end()) ? it_group->second : it_pid->second;
        int timeout = force ? 0 : processes_[process_name].graceful_timeout;
        return terminateUnixProcess(it_pid->second, group, timeout);
    }
#endif
    return false;
}

/**
 * @brief 检查进程是否正在运行
 *
 * @param process_name 进程名称
 * @return true 进程正在运行
 * @return false 进程未运行
 */
bool Process::isProcessRunning(const std::string &process_name) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it = process_handles_.find(process_name);
    if (it != process_handles_.end()) {
        return isWindowsProcessRunning(it->second);
    }
#else
    std::lock_guard<std::mutex> lock(process_mutex_);
    auto it = process_pids_.find(process_name);
    if (it != process_pids_.end()) {
        return isUnixProcessRunning(it->second);
    }
#endif
    return false;
}

// Windows 平台实现
#ifdef _WIN32

/**
 * @brief 创建Windows进程
 *
 * 在Windows平台上创建一个新的进程
 *
 * @param config 进程配置
 * @param proc_info 进程信息结构体
 * @return true 创建成功
 * @return false 创建失败
 */
bool Process::createWindowsProcess(ProcessConfig &config, PROCESS_INFORMATION &proc_info) {
    STARTUPINFOA startup_info;
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);
    ZeroMemory(&proc_info, sizeof(proc_info));

    std::string resolved_path = resolvePath(config.path, config.working_dir);
    std::string resolved_working_dir = config.working_dir.empty()
                                           ? resolved_path.substr(0, resolved_path.find_last_of("/\\"))
                                           : config.working_dir;

    std::string command_line = "\"" + resolved_path + "\" " + config.args;

    // 设置环境变量
    std::string env_block;
    if (!config.env_vars.empty()) {
        for (const auto &env: config.env_vars) {
            env_block += env.first + "=" + env.second + "\0";
        }
        env_block += "\0";
    }

    BOOL success = CreateProcessA(
        resolved_path.c_str(), // 应用程序路径
        const_cast<LPSTR>(command_line.c_str()), // 命令行
        NULL, // 进程安全属性
        NULL, // 线程安全属性
        FALSE, // 句柄继承选项
        CREATE_NEW_PROCESS_GROUP, // 创建标志 - 新进程组
        config.env_vars.empty() ? NULL : LPVOID(env_block.c_str()), // 环境变量
        resolved_working_dir.c_str(), // 工作目录
        &startup_info, // STARTUPINFO
        &proc_info // PROCESS_INFORMATION
    );

    if (!success) {
        log(LogLevel::ERROR, "CreateProcess failed for " + config.name +
                             ", Error: " + std::to_string(GetLastError()));
        return false;
    }

    // 创建作业对象来管理进程组
    HANDLE job = CreateJobObject(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {0};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &job_info, sizeof(job_info))) {
            log(LogLevel::WARNING, "Failed to set job object information for " + config.name);
        }
        if (!AssignProcessToJobObject(job, proc_info.hProcess)) {
            log(LogLevel::WARNING, "Failed to assign process to job object for " + config.name);
        }
        process_jobs_[config.name] = job;
    } else {
        log(LogLevel::WARNING, "Failed to create job object for " + config.name);
    }

    CloseHandle(proc_info.hThread);

    log(LogLevel::INFO, "Started Windows process: " + config.name +
                        " PID: " + std::to_string(proc_info.dwProcessId));
    return true;
}

/**
 * @brief 终止Windows进程
 *
 * @param proc_info 进程信息
 * @param timeout_ms 超时时间（毫秒）
 * @return true 终止成功
 * @return false 终止失败
 */
bool Process::terminateWindowsProcess(PROCESS_INFORMATION &proc_info, int timeout_ms) {
    if (!isWindowsProcessRunning(proc_info)) {
        return true;
    }

    // 先尝试优雅关闭
    if (timeout_ms > 0) {
        log(LogLevel::INFO, "Attempting graceful shutdown...");
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, proc_info.dwProcessId);

        DWORD result = WaitForSingleObject(proc_info.hProcess, timeout_ms);
        if (result == WAIT_OBJECT_0) {
            log(LogLevel::INFO, "Process terminated gracefully");
            return true;
        }
    }

    // 强制终止
    log(LogLevel::WARNING, "Force terminating process...");
    TerminateProcess(proc_info.hProcess, 0);
    WaitForSingleObject(proc_info.hProcess, 5000);

    cleanupWindowsProcess(proc_info);
    return true;
}

/**
 * @brief 检查Windows进程是否正在运行
 *
 * @param proc_info 进程信息
 * @return true 进程正在运行
 * @return false 进程未运行
 */
bool Process::isWindowsProcessRunning(PROCESS_INFORMATION &proc_info) {
    DWORD exit_code;
    if (GetExitCodeProcess(proc_info.hProcess, &exit_code)) {
        return exit_code == STILL_ACTIVE;
    }
    return false;
}

/**
 * @brief 清理Windows进程资源
 *
 * @param proc_info 进程信息
 */
void Process::cleanupWindowsProcess(PROCESS_INFORMATION &proc_info) {
    CloseHandle(proc_info.hProcess);
}

#else
// Unix/Linux 平台实现

/**
 * @brief 创建Unix进程
 *
 * 在Unix/Linux平台上通过fork创建一个新的进程
 *
 * @param config 进程配置
 * @param pid 进程ID（输出参数）
 * @return true 创建成功
 * @return false 创建失败
 */
bool Process::createUnixProcess(ProcessConfig &config, pid_t &pid) {
    std::string resolved_path = resolvePath(config.path, config.working_dir);
    std::string resolved_working_dir = config.working_dir.empty()
                                           ? resolved_path.substr(0, resolved_path.find_last_of("/"))
                                           : config.working_dir;

    pid = fork();

    if (pid == -1) {
        log(LogLevel::ERROR, "Fork failed for process: " + config.name +
                             " (errno: " + std::to_string(errno) + ")");
        return false;
    }

    if (pid == 0) {
        // 子进程
        // 设置进程组
        if (setsid() == -1) {
            std::cerr << "Failed to create new session for: " << config.name
                    << " Error: " << strerror(errno) << std::endl;
        }

        // 切换工作目录
        if (!resolved_working_dir.empty()) {
            if (chdir(resolved_working_dir.c_str()) != 0) {
                std::cerr << "Failed to change directory to: " << resolved_working_dir
                        << " Error: " << strerror(errno) << std::endl;
                exit(1);
            }
        }

        // 设置环境变量
        setupEnvironment(config.env_vars);

        // 解析参数
        std::vector<std::string> arg_list = splitArgs(config.args);
        std::vector<char *> argv;

        argv.push_back(const_cast<char *>(resolved_path.c_str()));
        for (auto &arg: arg_list) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // 执行程序
        execv(resolved_path.c_str(), argv.data());

        // 如果执行失败
        std::cerr << "Exec failed for: " << resolved_path
                << " Error: " << strerror(errno) << std::endl;
        exit(1);
    }

    log(LogLevel::INFO, "Started Unix process: " + config.name + " PID: " + std::to_string(pid));
    return true;
}

/**
 * @brief 设置进程组
 *
 * @param pid 进程ID
 * @param process_group 进程组ID（输出参数）
 * @return true 设置成功
 * @return false 设置失败
 */
bool Process::setupProcessGroup(pid_t pid, pid_t &process_group) {
    process_group = pid; // 使用进程ID作为进程组ID
    return true;
}

/**
 * @brief 终止Unix进程
 *
 * @param pid 进程ID
 * @param process_group 进程组ID
 * @param timeout_sec 超时时间（秒）
 * @return true 终止成功
 * @return false 终止失败
 */
bool Process::terminateUnixProcess(pid_t pid, pid_t process_group, int timeout_sec) {
    if (!isUnixProcessRunning(pid)) {
        return true;
    }

    // 先尝试优雅关闭
    if (timeout_sec > 0) {
        log(LogLevel::INFO, "Attempting graceful shutdown for PID: " + std::to_string(pid));

        // 发送SIGTERM到整个进程组
        if (kill(-process_group, SIGTERM) == -1) {
            log(LogLevel::WARNING, "Failed to send SIGTERM to process group: " + std::to_string(process_group) +
                                   " Error: " + strerror(errno));
        }

        // 等待进程结束
        for (int i = 0; i < timeout_sec * 10; ++i) {
            if (!isUnixProcessRunning(pid)) {
                log(LogLevel::INFO, "Process terminated gracefully");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // 强制终止
    log(LogLevel::WARNING, "Force terminating process group: " + std::to_string(process_group));
    if (kill(-process_group, SIGKILL) == -1) {
        log(LogLevel::ERROR, "Failed to send SIGKILL to process group: " + std::to_string(process_group) +
                             " Error: " + strerror(errno));
    }

    // 等待进程彻底结束
    std::this_thread::sleep_for(std::chrono::seconds(1));

    cleanupUnixProcess(pid);
    return true;
}

/**
 * @brief 检查Unix进程是否正在运行
 *
 * @param pid 进程ID
 * @return true 进程正在运行
 * @return false 进程未运行
 */
bool Process::isUnixProcessRunning(pid_t pid) {
    return kill(pid, 0) == 0;
}

/**
 * @brief 清理Unix进程资源
 *
 * @param pid 进程ID
 */
void Process::cleanupUnixProcess(pid_t pid) {
    // Unix 系统会自动清理，这里主要确保进程已经结束
    int status;
    waitpid(pid, &status, WNOHANG);
}

#endif

// 环境变量处理
/**
 * @brief 解析环境变量字符串
 *
 * 将逗号分隔的环境变量字符串解析为键值对映射
 *
 * @param env_str 环境变量字符串
 * @return std::map<std::string, std::string> 解析后的环境变量映射
 */
std::map<std::string, std::string> Process::parseEnvVars(const std::string &env_str) {
    std::map<std::string, std::string> env_vars;
    std::istringstream iss(env_str);
    std::string pair;

    while (std::getline(iss, pair, ',')) {
        size_t pos = pair.find('=');
        if (pos != std::string::npos) {
            std::string key = pair.substr(0, pos);
            std::string value = pair.substr(pos + 1);

            // 处理环境变量引用（如 ${VAR}）
            size_t var_start;
            while ((var_start = value.find("${")) != std::string::npos) {
                size_t var_end = value.find("}", var_start);
                if (var_end != std::string::npos) {
                    std::string var_name = value.substr(var_start + 2, var_end - var_start - 2);
                    const char *var_value = std::getenv(var_name.c_str());
                    if (var_value) {
                        value.replace(var_start, var_end - var_start + 1, var_value);
                    } else {
                        // 保留未定义的变量引用或者用空字符串替代
                        value.replace(var_start, var_end - var_start + 1, "");
                        log(LogLevel::WARNING, "Environment variable " + var_name + " not defined");
                    }
                } else {
                    // 未闭合的大括号，跳出循环避免无限循环
                    break;
                }
            }

            env_vars[key] = value;
        }
    }

    return env_vars;
}

/**
 * @brief 设置环境变量
 *
 * 根据配置设置子进程的环境变量
 *
 * @param env_vars 环境变量映射
 */
void Process::setupEnvironment(const std::map<std::string, std::string> &env_vars) {
#ifdef _WIN32
    // Windows 环境变量在 CreateProcess 中设置
#else
    for (const auto &env: env_vars) {
        if (setenv(env.first.c_str(), env.second.c_str(), 1) != 0) {
            log(LogLevel::WARNING, "Failed to set environment variable: " + env.first);
        }
    }
#endif
}

// 路径解析
/**
 * @brief 解析路径
 *
 * 根据工作目录解析相对路径为绝对路径
 *
 * @param path 路径
 * @param working_dir 工作目录
 * @return std::string 解析后的路径
 */
std::string Process::resolvePath(const std::string &path, const std::string &working_dir) {
    if (path.empty()) return path;

    // 如果是绝对路径，直接返回
    if (path[0] == '/' || (path.length() > 1 && path[1] == ':')) {
        return path;
    }

    // 相对路径，结合工作目录
    if (!working_dir.empty()) {
        std::string resolved = working_dir;
        if (resolved.back() != '/' && resolved.back() != '\\') {
            resolved += '/';
        }
        resolved += path;
        return resolved;
    }

    return path;
}

// 配置验证
/**
 * @brief 验证进程配置
 *
 * 检查进程配置是否有效
 *
 * @param config 进程配置
 * @return true 配置有效
 * @return false 配置无效
 */
bool Process::validateProcessConfig(const ProcessConfig &config) {
    if (config.path.empty()) {
        log(LogLevel::ERROR, "Process path is empty for: " + config.name);
        return false;
    }

    std::string resolved_path = resolvePath(config.path, config.working_dir);
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

// 文件系统操作
/**
 * @brief 创建目录
 *
 * 创建指定路径的目录（如果不存在）
 *
 * @param path 目录路径
 * @return true 创建成功或目录已存在
 * @return false 创建失败
 */
bool Process::createDirectory(const std::string &path) {
    if (path.empty()) return true;

#ifdef _WIN32
    return CreateDirectoryA(path.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

/**
 * @brief 检查文件是否存在
 *
 * @param path 文件路径
 * @return true 文件存在
 * @return false 文件不存在
 */
bool Process::fileExists(const std::string &path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0 && S_ISREG(buffer.st_mode));
#endif
}

// 日志系统
/**
 * @brief 记录日志
 *
 * 根据日志级别记录日志信息到控制台和文件
 *
 * @param level 日志级别
 * @param message 日志消息
 */
void Process::log(LogLevel level, const std::string &message) {
    if (level < log_level_) return;

    std::string level_str;
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

    std::string log_message = "[" + getCurrentTime() + "] [" + level_str + "] " + message;

    // 控制台输出
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::cout << log_message << std::endl;
    }

    // 文件输出
    if (!log_file_.empty()) {
        logToFile(log_message);
    }
}

/**
 * @brief 将日志写入文件
 *
 * @param message 日志消息
 */
void Process::logToFile(const std::string &message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    std::ofstream file(log_file_, std::ios_base::app);
    if (file.is_open()) {
        file << message << std::endl;
    }
}

/**
 * @brief 设置日志级别
 *
 * @param level 日志级别
 */
void Process::setLogLevel(LogLevel level) {
    log_level_ = level;
}

// 工具函数
/**
 * @brief 分割命令行参数
 *
 * 将命令行参数字符串分割为参数列表
 *
 * @param args 参数字符串
 * @return std::vector<std::string> 参数列表
 */
std::vector<std::string> Process::splitArgs(const std::string &args) {
    std::vector<std::string> result;
    std::istringstream iss(args);
    std::string token;
    bool in_quotes = false;
    std::string current_arg;

    for (char c: args) {
        if (c == '\"') {
            in_quotes = !in_quotes;
        } else if (std::isspace(c) && !in_quotes) {
            if (!current_arg.empty()) {
                result.push_back(current_arg);
                current_arg.clear();
            }
        } else {
            current_arg += c;
        }
    }

    if (!current_arg.empty()) {
        result.push_back(current_arg);
    }

    return result;
}

/**
 * @brief 获取当前时间字符串
 *
 * @return std::string 格式化的时间字符串
 */
std::string Process::getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&time_t));

    std::string result(buffer);
    result += "." + std::to_string(ms.count());
    return result;
}
