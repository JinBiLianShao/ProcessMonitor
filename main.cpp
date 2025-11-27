#include "process.h"
#include <iostream>
#include <csignal>
#include <atomic>

std::atomic<bool> running{true};
Process *g_daemon = nullptr;


/**
 * @brief 信号处理回调函数，用于捕获系统信号并优雅关闭程序。
 *
 * 当接收到如 SIGINT、SIGTERM 等终止信号时，该函数会设置运行标志为 false，
 * 并调用全局 daemon 的 stopAll 方法停止所有子进程。对于某些致命信号（如 SIGABRT 和 SIGSEGV），
 * 将直接调用 abort 终止程序。
 *
 * @author jinbilianshao
 *
 * @param signal 接收到的信号编号
 */
void signalHandler(int signal) {
    std::cout << "Received signal: " << signal << ". Shutting down..." << std::endl;
    running = false;

    if (g_daemon) {
        g_daemon->stopAll();
    }

    // 对于某些致命信号，直接退出
    if (signal == SIGABRT || signal == SIGSEGV) {
        std::abort();
    }
}

/**
 * @brief 注册需要处理的系统信号处理器。
 *
 * 包括 SIGINT（Ctrl+C）、SIGTERM（kill 默认信号），在非 Windows 平台上还注册了 SIGQUIT，
 * 同时忽略 SIGPIPE 信号以防止因写入已关闭管道导致程序异常退出。
 */
void setupSignalHandlers() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#ifndef _WIN32
    signal(SIGQUIT, signalHandler);
    signal(SIGPIPE, SIG_IGN); // 忽略管道破裂信号
#endif
}

/**
 * @brief 程序主入口点。
 *
 * 负责解析命令行参数、加载配置文件、启动进程监控，并进入主事件循环等待退出信号。
 * 支持通过 -c 或 --config 指定配置文件路径，使用 -h 或 --help 显示帮助信息。
 *
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 成功返回 0，失败返回非零错误码
 */
int main(int argc, char *argv[]) {
    std::string config_file = "process_config.ini";

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                std::cerr << "Error: Config file path not specified" << std::endl;
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -c, --config <file>  Specify config file (default: config.ini)" << std::endl;
            std::cout << "  -h, --help           Show this help message" << std::endl;
            return 0;
        }
    }

    // 设置信号处理
    setupSignalHandlers();

    Process daemon;
    g_daemon = &daemon;

    // 加载配置文件
    if (!daemon.loadConfig(config_file)) {
        std::cerr << "Failed to load config file: " << config_file << std::endl;
        return 1;
    }

    std::cout << "Starting process daemon..." << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;

    // 启动所有进程监控
    if (!daemon.startAll()) {
        std::cerr << "Failed to start process monitoring" << std::endl;
        return 1;
    }

    // 主循环
    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 停止所有进程
    std::cout << "Shutting down process daemon..." << std::endl;
    daemon.stopAll();

    std::cout << "Process daemon stopped successfully" << std::endl;
    return 0;
}
