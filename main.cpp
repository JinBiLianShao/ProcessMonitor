#include "process.h"
#include <iostream>
#include <csignal>
#include <atomic>

std::atomic<bool> running{true};
Process *g_daemon = nullptr;

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

void setupSignalHandlers() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#ifndef _WIN32
    signal(SIGQUIT, signalHandler);
    signal(SIGPIPE, SIG_IGN); // 忽略管道破裂信号
#endif
}

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
