# FramePlayer - 二进制图像帧播放器

基于 **Qt 5 + C++14** 的桌面 GUI 程序，用于读取和实时播放**原始二进制图像帧序列**。文件按固定格式（RGB888 / RGB565 / 灰度8位）连续存放图像帧，无文件头、无压缩，FramePlayer 根据用户设定的宽高和像素格式将数据切分为帧并播放。

---

## 功能概览

| 能力 | 说明 |
|------|------|
| 像素格式 | RGB888（3B/像素）、RGB565（2B/像素）、灰度8位（1B/像素） |
| 数据源 | 5 种模式：本地文件（主线程）、本地文件（QThread 子线程）、本地文件（纯 C++ 线程）、模拟数据（主线程）、模拟数据（纯 C++ 线程） |
| 文件加载 | 打开文件对话框 或 拖拽文件到窗口 / 图像区；保持文件句柄打开，按帧 seek 读取，**支持超大文件** |
| 播放控制 | 开始 / 暂停·继续 / 停止 三态；进度条拖动跳转；到末尾自动回卷循环 |
| 帧率 | 可设 1–240 fps，左上角显示**实测帧率** |
| 图像显示 | 等比缩放居中、黑底、零拷贝渲染、缩放结果缓存 |
| 日志 | 自动分配控制台窗口实时显示操作 / 计数 / 帧率；同时写入 `logs/yyyyMMdd_FramePlayer.log`（5MB × 3 滚动） |

---

## 五种数据源模式

| 模式 | 线程模型 | 说明 |
|------|----------|------|
| **本地文件** | 主线程 QTimer | 主线程按帧 seek 读取，简单直接 |
| **本地文件（子线程）** | QThread 子线程 | 子线程 `while + msleep` 顺序读帧，信号发回主线程显示，播放不阻塞 UI |
| **本地文件（纯C++线程）** | std::thread + SPSC 无锁环 | 完全不依赖 Qt 的纯 C++ 子线程读帧，零拷贝帧槽缓冲池（4 槽），可独立移植 |
| **模拟数据** | 主线程 QTimer | 按设定格式/宽高实时生成移动渐变测试帧，用于验证各格式渲染 |
| **模拟数据（纯C++线程）** | std::thread + SPSC 无锁环 | 纯 C++ 子线程生成**流线型动态粒子**（伪流场 + 拖尾衰减 + HSV 着色），画面更具动态感 |

---

## 架构与关键设计

### 核心模块

```
main.cpp               程序入口，初始化日志，创建主窗口
FramePlayer.*          主窗口，播放状态机、数据源切换、播放控制、帧获取
FrameViewWidget.*      图像显示控件，零拷贝渲染、缩放缓存、实测帧率统计
FrameReadThread.*      QThread 子线程读帧器
CppFrameSource.*       纯 C++ 子线程帧数据源（零拷贝、SPSC 无锁、帧槽池）
SpscRingBuffer.h       单生产者单消费者无锁环形缓冲区（模板类，无依赖）
Logger.h               全局日志封装（spdlog：控制台彩色 + 滚动文件，Qt 日志自动汇入）
```

### 零拷贝策略

- **帧数据层**：`QImage` 直接浅包装外部缓冲，避免大帧（如 2048×2048×3 = 12 MB）每帧深拷贝
- **纯 C++ 模式**：4 个预分配的帧槽缓冲池，生产者填充槽后仅移交槽所有权（SPSC 环），消费者 `acquireFrame()` 取走、`releaseFrame()` 归还，运行期**零内存分配**
- **QThread 模式**：`QByteArray` 隐式共享，跨线程拷贝开销极小

### 线程安全

- 控制量（停止 / 暂停 / 帧率 / 跳转）全部走 `std::atomic`，任意线程可随时调用，无需加锁
- 帧槽经 SPSC 环做所有权移交，同一时刻只有一个线程访问某个槽
- 两种子线程均在 `while + sleep` 中以 10 ms 小步睡眠，及时响应停止 / 暂停 / 帧率调整 / 跳转

### SpscRingBuffer 设计要点

- 容量自动向上取整为 2 的幂，位运算替代取模
- 本地缓存（`headCached_` / `tailCached_`）减少跨核原子读取
- `alignas(64)` 分离生产者 / 消费者热路径数据，避免伪共享
- 仅要求平凡析构类型，无需显式析构

---

## 编译与运行

### 环境要求

- **Windows**：Visual Studio 2022（MSVC v143）
- **Qt 5.14.2**（MSVC 2017 编译器），模块：core / gui / widgets
- **spdlog**（header-only，已内置于 `spdlog/` 目录）
- C++14 标准

### 构建

直接双击 `FramePlayer.sln` 用 Visual Studio 打开，选择 **Win32 / Debug 或 Release** 配置，生成即可。

项目预定义了：
- `/utf-8` 编译选项（源码与控制台 UTF-8）
- `SPDLOG_WCHAR_FILENAMES`（中文路径安全）
- `/std:c++14`（MSVC v143 默认）

### 运行

编译后运行 `Debug/FramePlayer.exe`（或 Release 目录下的对应文件）。程序启动时自动弹出控制台窗口显示实时日志，GUI 窗口标题为 **"FramePlayer - 二进制图像播放器"**。

---

## 使用说明

### 基本流程

1. **设置图像参数**（右侧面板）：选择像素格式（RGB888 / RGB565 / 灰度8位）、输入宽高
2. **选择数据源**：5 种 RadioButton 模式任选
3. **加载或生成帧**：
   - 文件模式：点击「打开文件...」或直接拖拽文件到左侧图像区
   - 模拟模式：在「模拟帧数」设置总帧数（默认 300）
4. **播放控制**：点击「开始播放」→「暂停 / 继续」→「停止」，拖动进度条跳转
5. **帧率调整**：拖动右侧「帧率」微调框（1–240 fps），运行时可调

### 快速上手（测试）

项目内置测试数据 `TestData/`，可直接打开验证各格式：

| 文件 | 对应设置 |
|------|----------|
| `test_rgb888_320x240_60f.bin` | 格式：RGB888，宽 320，高 240 |
| `test_rgb565_320x240_60f.bin` | 格式：RGB565，宽 320，高 240 |
| `test_gray8_320x240_60f.bin`  | 格式：灰度8位，宽 320，高 240 |

画面应显示**水平滚动的彩色渐变**，证明播放正常。

---

## 生成测试数据

`TestData/gen_test_frames.py` 可重新生成测试二进制文件：

```bash
python gen_test_frames.py              # 生成全部三种格式
python gen_test_frames.py rgb888       # 仅生成 RGB888
python gen_test_frames.py rgb565       # 仅生成 RGB565
python gen_test_frames.py gray8        # 仅生成灰度8位
```

---

## 项目结构

```
FramePlayer/
├── main.cpp                       # 程序入口
├── FramePlayer.h / .cpp / .ui    # 主窗口（状态机 + 播放控制 + 帧获取）
├── FramePlayer.qrc / app.ico     # Qt 资源（图标）
├── FrameViewWidget.h / .cpp      # 图像显示控件（零拷贝 + 缩放缓存 + FPS 统计）
├── FrameReadThread.h / .cpp      # QThread 子线程读帧器
├── CppFrameSource.h / .cpp       # 纯 C++ 子线程帧数据源（SPSC 无锁 + 帧槽池 + 粒子模拟）
├── SpscRingBuffer.h              # 无锁环形缓冲区（模板类，无外部依赖）
├── Logger.h                      # 日志封装（spdlog：控制台 + 滚动文件 + Qt 日志转发）
├── FramePlayer.vcxproj           # Visual Studio 项目
├── FramePlayer.sln               # Visual Studio 解决方案
├── spdlog/                       # spdlog header-only 库
└── TestData/
    ├── gen_test_frames.py        # 测试数据生成脚本
    ├── test_rgb888_320x240_60f.bin
    ├── test_rgb565_320x240_60f.bin
    └── test_gray8_320x240_60f.bin
```

---

## 日志

程序启动后：
- **控制台**：实时彩色输出操作事件、播放计数、实测帧率
- **文件**：`logs/yyyyMMdd_FramePlayer.log`（5 MB 自动滚动，保留 3 份）

Qt 环境下的 `qDebug` / `qWarning` 等日志会自动汇入 spdlog 统一输出。

---

## 技术栈

- **Qt 5.14.2**（Widgets / GUI / Core）
- **C++14**（MSVC v143）
- **spdlog**（header-only，日志）
- **Win32 API**（控制台分配、宽字符路径）
