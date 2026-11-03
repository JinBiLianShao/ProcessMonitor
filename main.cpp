/**
 * @file main.cpp
 * @brief 入口。干的活很少：解析命令行、屏蔽信号、把活交给 Process。
 * @author jinbilianshao
 *
 * 信号这块是重写过的，说一下为什么这么写：
 *
 * 老版本在 signal handler 里直接调 stopAll()，而 stopAll 会 join 线程、拿互斥锁。
 * 这两件事在信号处理函数里都是不能碰的 —— handler 可能打断任何线程，包括正拿着
 * log_mutex_ 的那个；这时候再去抢锁就是自己锁自己，进程就挂在那儿不动了。
 *
 * 现在改成：创建线程【之前】先把 SIGINT/SIGTERM/SIGQUIT 屏蔽掉（这样所有线程都继承
 * 这个屏蔽字，信号不会跑到某个线程里去执行 handler），然后在主循环里用 sigtimedwait
 * 同步地把信号取出来。这样 stopAll() 只会在主线程里、且只调一次。
 *
 * 副作用提醒：屏蔽字会被 fork 出来的子进程继承。子进程那边必须在 exec 之前
 * 把屏蔽字清掉，否则它永远收不到 SIGTERM —— 这件事在 process.cpp 的
 * createUnixProcess 里做了，改的时候别漏。
 */

#include "process.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

// 默认配置文件名。以前这里写的是 process_config.ini，但仓库里的文件叫 config.ini，
// 帮助文本也写 config.ini，结果 ./ProcessMonitor 直接起不来。统一成这个。
constexpr const char *kDefaultConfig = "config.ini";

void printUsage(const char *argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "Options:\n"
              << "  -c, --config <file>  Specify config file (default: " << kDefaultConfig << ")\n"
              << "  -h, --help           Show this help message\n"
              << "  -v, --version        Show version\n";
}

} // namespace

int main(int argc, char *argv[]) {
    std::string config_file = kDefaultConfig;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "Error: config file path not specified" << std::endl;
                return 1;
            }
            config_file = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            std::cout << "ProcessMonitor 1.1.0" << std::endl;
            return 0;
        } else {
            // 不认识的参数直接报错退出，别猜用户想干嘛
            std::cerr << "Error: unknown option '" << arg << "'" << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

#ifndef _WIN32
    // 关键一步：必须在【任何线程创建之前】屏蔽信号，之后创建的线程才会继承这个屏蔽字
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGQUIT);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
        std::cerr << "Error: failed to block termination signals: " << strerror(errno) << std::endl;
        return 1;
    }
    // 写日志的时候对端可能已经关了，默认行为是直接把自己搞死，这里忽略掉
    signal(SIGPIPE, SIG_IGN);
#else
    // Windows 没有 sigtimedwait，用一个原子标志 + 控制台回调凑合
    static std::atomic<bool> stop_requested{false};
    SetConsoleCtrlHandler([](DWORD) -> BOOL {
        stop_requested = true;
        return TRUE;
    }, TRUE);
#endif

    Process daemon;

    if (!daemon.loadConfig(config_file)) {
        std::cerr << "Failed to load config file: " << config_file << std::endl;
        return 1;
    }

    if (!daemon.startAll()) {
        std::cerr << "Failed to start process monitoring" << std::endl;
        return 1;
    }

    std::cout << "Process daemon is running. Send SIGINT/SIGTERM to stop." << std::endl;

#ifndef _WIN32
    // 200ms 轮询一次。没有用 pause() 是因为它会被任何信号唤醒，
    // 而我们要的是"只认这三个信号"。
    const timespec poll_interval{0, 200 * 1000 * 1000};
    while (true) {
        const int signo = sigtimedwait(&stop_signals, nullptr, &poll_interval);
        if (signo == SIGINT || signo == SIGTERM || signo == SIGQUIT) {
            std::cout << "Received signal " << signo << " (" << strsignal(signo)
                      << "), shutting down..." << std::endl;
            break;
        }
        // -1 且 errno 是 EAGAIN 表示只是超时，很正常，继续转
        if (signo == -1 && errno != EAGAIN && errno != EINTR) {
            std::cerr << "sigtimedwait failed: " << strerror(errno) << std::endl;
            break;
        }
    }
#else
    while (!stop_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "Shutdown requested, stopping..." << std::endl;
#endif

    // 这里调一次就够了。析构函数里还会再调一次，但 stopAll 是幂等的，
    // 第二次直接返回 —— 以前没做幂等，结果同一套流程跑了三遍，日志都重复三份。
    daemon.stopAll();
    std::cout << "Process daemon stopped." << std::endl;
    return 0;
}
