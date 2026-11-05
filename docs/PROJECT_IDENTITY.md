# 项目命名

确定日期：2026-11-05。

## 名称与用途

- 英文项目名：**Aevotrace**。
- 中文名：**溯迹**。
- 仓库短名：`aevotrace`。
- 官方仓库：[PeterWind6174/aevotrace](https://github.com/PeterWind6174/aevotrace)，2026-11-05 创建，公开可见。
- 将来的 CLI、C++ namespace 或包前缀优先使用 `aevotrace`；当前尚未实现或发布。
- 简介：Developer-oriented cross-architecture, cross-OS binary conversion, starting with Windows x86-64 to macOS ARM64.
- 中文简介：面向开发者的跨架构、跨系统二进制转换工具。

2026-11-05 需求讨论后，范围由 AArch64 专项研究扩展为通用平台组合，首版改为 Windows x64 → macOS ARM64；名称保留。

名称为项目造词，以 `trace` 对应执行记录，“溯迹”表达从运行痕迹追溯代码的方向。名称不绑定某款游戏、某个模拟器或单一技术后端，便于后续调整范围。

## 重名核查

2026-11-05 通过 GitHub 连接器进行仓库搜索：

| 查询 | 公开检索结果 | 处理 |
| --- | --- | --- |
| `Tracelume in:name` | 有结果 | 排除，网页也存在同名产品 |
| `Ichnora in:name` | 有结果 | 排除 |
| `Aevotrace in:name` | 无结果 | 保留 |
| `aevotrace` | 无结果 | 保留 |
| 网页精确检索 `"Aevotrace"` | 无结果 | 作为补充核查 |

可复核的 [GitHub 仓库检索](https://github.com/search?q=aevotrace&type=repositories)。搜索结果会随时间变化，且可能受索引延迟和可见性限制；未发现结果不代表全球无重名、商标可注册或域名可用，也不会预留 GitHub 仓库名。

初次命名核查时未创建远程仓库；同日后续已创建上列 GitHub public 仓库。未注册域名，也未进行商标检索。中文名用于辅助表达，未据此宣称唯一。

## 对外表述

使用“研究原型”“execution-guided AOT”“source-available”等与当前事实一致的描述。尚未实现时不使用兼容性清单、加速倍数或“通用 Switch 原生化工具”等表述。
