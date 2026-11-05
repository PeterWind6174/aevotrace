# P0-BUILD-REPRO-01：ENGINE/UI 构建可复现口径

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾；R06/证据质量；前置 ENGINE `a4ef11c`、UI `847dad7`。
- owner/reviewer/QA：compiler（lead）+ qa / reviewer / qa。
- 契约：P1-CONTRACT 的 artifact/测试结果字段。

## 目标与范围

- 统一“可复现”定义：输入源码/工具版本、固定 `.text` 哈希、完整二进制哈希、构建目录/install name 影响。
- UI 已用 `/Brepro`；ENGINE 的 dylib/runner 因 install name/绝对路径变化，需固定 RPATH/构建目录或记录预期差异。
- 产出脚本/文档，使同一 commit 在干净目录可重复得到相同哈希或明确的路径相关差异说明。
- 不包含：跨机器绝对可复现、签名/公证、性能。

## 验收

- 同一机器、同一工具链、两个干净构建目录：记录哪些哈希相同、哪些因路径不同；给出可接受判据。
- QA 复跑一致；报告不把“构建成功”当“可复现”。
- P1 artifact manifest 采用该口径。

## 完成报告

- 命令/退出码、哈希矩阵、差异原因、未测项。
