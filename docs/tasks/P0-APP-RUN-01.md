# P0-APP-RUN-01：7-Zip 26.05 Windows 参考工作流

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾/P0-01；R02/R04；前置 P0-SCOPE-01 候选与 fixture。
- owner/reviewer/QA：qa / reviewer / qa（本任务就是参考运行）。
- 输入：7-Zip 26.05 `7zFM.exe`+`7z.dll`（包 `fd99e86f...`，主模块哈希见 SCOPE 报告）；fixture `b1f05255...`。
- 工作目录：`C:\Users\PeterWind\AevotraceP0\P0-APP-RUN-01`；不安装、不改注册表/Shell 扩展。

## 目标与范围

- 在 Windows 交互桌面执行固定工作流：启动 → 打开 fixture → 列出 4 文件/2 目录 → 解压 → 校验 4 个成员 SHA-256 → 退出。
- 记录版本/哈希、窗口/控件/菜单、进程树（是否 `7zG.exe`）、退出码、输出哈希、截图；不要求已转换。
- 明确排除 Shell 右键菜单/拖放/UNC/密码/多卷/安装器。

## 验收

- 工作流可重复；输出哈希与 fixture manifest 一致；Unicode 文件名正确。
- 可见窗口/模态/焦点由用户或 QA 在会话 1 观察；未完成标待人工。
- 失败/未触发路径记录，不用 `7z.exe` 命令行替代 GUI 证据。

## 完成报告

- 命令/退出码、截图/进程树、哈希、未测项、对首版范围的建议。
