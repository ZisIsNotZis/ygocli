# ygocli

**[English](README.md) | 中文**

**一个极简、文本优先的游戏王对战 CLI 客户端**，基于 `ocgcore`，启动快、便于调试，也适合观察引擎行为和收集对局日志。

## 30 秒速览

- **它是什么：** 单文件 CLI 对战运行器（`./ygocli <deck0.ydk> <deck1.ydk> [--auto] [--random]`）
- **现在能做什么：** 读卡组、启动对局、显示场面状态、处理大量 `MSG_*`、支持手动与 `--auto` 出牌
- **还比较粗糙的地方：** 若干响应编码边界、协议覆盖还不完整、长局稳定性仍有限
- **最适合做什么：** 本地快速实验、协议/解析调试、生成分析日志

## 当前状态

| 项目 | 状态 |
| --- | --- |
| 构建 | ✅ `make` |
| 基础对局启动 | ✅ |
| 自动对局推进 | ✅（已观察到多回合） |
| 战斗完整性 | ⚠️ 某些场景不稳定 |
| 协议覆盖 | ⚠️ 较广但未完整 |
| 长局稳定性 | ❌ 部分对局仍可能进入 retry loop |

### `example.log` 的现象

- 已进入正常对局流程（`DRAW`、`NEW_TURN`、`NEW_PHASE`、`SELECT_*`、`MOVE`、`SET` 等）。
- 存在 **5** 次 `MSG_UNKNOWN`（未处理消息类型 0）。
- 存在 **111,890** 次 `MSG_RETRY`，且没有干净结束标记（`MSG_WIN` / `PROCESSOR_END`）——说明仍有 retry-loop 问题。

## 快速开始

### Ubuntu 推荐依赖

```bash
# 开发依赖
sudo apt install -y \
  freeglut3-dev \
  libfreetype-dev \
  libirrlicht-dev \
  libjpeg-dev \
  liblzma-dev \
  liblua5.3-dev \
  libminiaudio-dev \
  libsqlite3-dev

# 常用工具
sudo apt install -y sqlite3

# WindBot 需要的最小 mono 库
sudo apt install -y \
  libmono-system-data4.0-cil \
  libmono-system-runtime-serialization4.0-cil
```

```bash
# 构建
make

# 手动运行
./ygocli deck/260124DD/b7fb7204fab3c94d.ydk deck/260124DD/b7fb7204fab3c94d.ydk

# 自动决策
./ygocli deck/260124DD/b7fb7204fab3c94d.ydk deck/260124DD/b7fb7204fab3c94d.ydk --auto

# 随机自动决策
./ygocli deck/260124DD/b7fb7204fab3c94d.ydk deck/260124DD/b7fb7204fab3c94d.ydk --random

# 定向回归测试
make test-regression

# 随机 fuzz 测试（随机卡组 + 随机选择）
make test-fuzz-random
```

## 已完成

- 基于 Makefile 的构建流程（`make`）。
- `ygocli` 构建路径已移除 `gframe` 依赖。
- 基于消息驱动的对局循环，支持场面展示和交互式选择。
- 支持读取 `.ydk` 卡组（`#main`、`#extra`、遇到 `!side` 停止）。
- 增加 `YGOCLI_SEED`，方便复现测试/对局。

