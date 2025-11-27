# ProcessMonitor

进程监控守护程序，用于监控和管理多个子进程。支持跨平台（Windows/Linux）进程管理，提供配置加载、进程启动/停止、状态监控、僵尸进程清理等功能。

## 功能特性

- **多进程管理**：同时监控和管理多个子进程
- **自动重启**：进程异常退出时根据配置自动重启
- **跨平台支持**：支持Windows和Linux平台
- **配置驱动**：通过INI配置文件灵活配置各项参数
- **日志记录**：支持多级别的日志记录到文件和控制台
- **优雅关闭**：支持进程的优雅关闭和强制终止
- **僵尸进程清理**：自动清理已终止但仍占用系统资源的子进程

## 安装与构建

### 环境要求

- C++17兼容的编译器（GCC 7+/Clang 5+/MSVC 2017+）
- CMake 3.10或更高版本
- POSIX兼容系统（Linux/Unix）或Windows系统

### 构建步骤

```bash
# 克隆项目
git clone <repository-url>
cd ProcessMonitor

# 创建构建目录
mkdir build && cd build

# 配置项目
cmake ..

# 编译项目
make -j
```


### CMake配置选项

- `COMPILER_TARGET`：目标架构（x86/arm32/arm64）
- `PUBLIC_PACKAGE_DIR`：第三方库路径
- `CMAKE_BUILD_TYPE`：构建类型（Release/Debug）
- `TARGET_TYPE`：目标类型（executable/static/shared）

## 配置文件

程序使用INI格式的配置文件，包含全局配置和进程配置两部分。

### 全局配置

```ini
[Global]
log_level=INFO           # 日志级别(DEBUG/INFO/WARNING/ERROR)
log_file=process.log     # 日志文件路径
check_interval=2         # 进程检查间隔（秒）
```


### 进程配置

```ini
[Process1]
name=demo                # 进程名称
path=/path/to/executable # 可执行文件路径
working_dir=/work/dir    # 工作目录
args=                    # 命令行参数
env_vars=                # 环境变量（逗号分隔的键值对）
restart_delay=3          # 重启延迟（秒）
max_restarts=5           # 最大重启次数（-1表示无限重启）
graceful_timeout=10      # 优雅关闭超时时间（秒）
```


## 使用方法

### 启动程序

```bash
# 使用默认配置文件启动
./ProcessMonitor

# 指定配置文件启动
./ProcessMonitor -c /path/to/config.ini

# 显示帮助信息
./ProcessMonitor -h
```


### 运行时控制

- **正常退出**：按Ctrl+C或发送SIGINT/SIGTERM信号
- **强制退出**：发送SIGKILL信号（不推荐）

## 核心组件

### Process类

主控制器类，负责：
- 配置文件加载与解析
- 进程生命周期管理
- 状态监控与日志记录
- 跨平台进程操作封装

### ProcessConfig结构体

存储单个进程的配置信息：
- 基本属性：名称、路径、工作目录、命令行参数
- 环境变量配置
- 重启策略配置
- 状态标志（原子变量保证线程安全）

### 线程模型

采用多线程架构：
- **主线程**：负责配置加载和总体控制
- **监控线程**：每个进程对应一个监控线程
- **清理线程**：每个进程对应一个僵尸清理线程

## 平台差异

### Windows平台

使用Windows API进行进程管理：
- `CreateProcess`创建进程
- `TerminateProcess`终止进程
- Job对象管理进程组

### Linux/Unix平台

使用POSIX API进行进程管理：
- `fork`/`exec`创建进程
- `kill`发送信号控制进程
- `waitpid`清理僵尸进程

## 日志系统

支持4个日志级别：
- **DEBUG**：调试信息
- **INFO**：普通信息
- **WARNING**：警告信息
- **ERROR**：错误信息

日志同时输出到控制台和文件，可通过配置文件调整日志级别。

## 故障排除

### 常见问题

1. **进程无法启动**
    - 检查可执行文件路径是否正确
    - 确认文件具有执行权限
    - 检查依赖库是否完整

2. **进程频繁重启**
    - 检查程序是否有运行时错误
    - 调整restart_delay参数
    - 查看详细日志定位问题

3. **僵尸进程未清理**
    - 检查清理线程是否正常运行
    - 确认信号处理是否正确

### 调试建议

- 设置`log_level=DEBUG`获取详细日志
- 使用`strace`（Linux）或Process Monitor（Windows）跟踪进程行为
- 检查系统资源限制（如文件描述符数量）

## 许可证

本项目采用MIT许可证，详情请参见LICENSE文件。