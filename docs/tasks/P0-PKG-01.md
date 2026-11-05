# P0-PKG-01：最小 ARM64 独立启动探针

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾/R05；前置 P0-ENGINE-01 `a4ef11c`、P0-UI-01 `847dad7`。
- owner/reviewer/QA：platform（lead）+ qa / reviewer / qa。
- 基线/契约：ENGINE/UI spike；P1-CONTRACT 草案。

## 目标与范围

- 构造最小 ARM64 `.app`（或等价可启动包）：主程序 + 运行时 + 模式后端 + AOT 模块，使用 `@rpath`/随包 dylib，不依赖 Homebrew 路径。
- 本地 ad-hoc 签名/`codesign --verify`；JIT 路径按需 `com.apple.security.cs.allow-jit`；可写缓存目录在 Application Support/Caches。
- 在未安装 Homebrew/LLVM/Capstone 的干净 Mac 上启动并运行一条 GUI/回调路径；检查 `lipo -archs`/`otool -L`/`spctl`。
- 不包含：正式分发签名/公证/App Store；真实应用；性能。

## 验收

- 包内执行链无 Rosetta、无开发机路径；`otool -L` 仅系统/随包库；启动业务状态可验证。
- 签名/entitlement/可写目录证据；未完成项标待人工/未测。
- 失败时保留原始日志，不把包装当转换成功。

## 完成报告

- 包结构、命令/退出码、哈希、签名/依赖检查、未测项。
