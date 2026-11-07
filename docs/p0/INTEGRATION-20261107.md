# 2026-11-07：本批统一合并与收尾

状态：代码已统一到 main；主工作树 CTest 通过；最终 GUI 验证及归档后移除正在本次收尾中执行。此处只收口已分配工作，不宣布 P0 总体验收或首版产品完成，下一阶段尚未启动。

## 代码与文档

- 原 main：`c91a3f7e78e34e0d5e224d54f286e5d2e1bbc4a7`。
- 已快进集成：`ce9c11b0282375aec79ca7fcf01f4bf1f0c097eb`；最终产品源码：`47b2b7a86f15ee67274fcf613a352e38791ec140`。
- 盘点的全部本地任务分支和 19 个附加 worktree HEAD 都是该集成提交的祖先，无独立未合并提交；附加 worktree 全部干净。
- 根目录 9 份待提交文档先逐文件备份；8 份内容已与集成版本一致，唯一新增 session-fix 提示词恢复并纳入本次文档收尾。
- 更新 README、AGENTS、当前状态、支持矩阵、ADR、开发/协作入口和任务索引；保留旧报告的历史结论，消除旧工作树/未派发状态误导。
- 非阻断加固项及真正未完成的产品门槛转入 [待办清单](../BACKLOG.md)。

## 主工作树验证

规范构建目录 `.local/build-main`，来自根目录 main 源码，无已归档 worktree 依赖：

```sh
cmake -S spikes/p0-engine -B .local/build-main -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build .local/build-main -j 4
ctest --test-dir .local/build-main --output-on-failure
```

configure/build/ctest 均退出 0，CTest **8/8**（46.72 秒）；PE session **267/267**、wrapper **436/436**、PE DLL **149/149**、语义 **791**、Trace/AOT **78**。日志 `.local/build-main-{configure,build,ctest}.log` 及 build 内各结果目录。

最终 GUI 验证使用 `.local/venv-callback/bin/python`，顺序运行，记录在 `.local/results/CLOSEOUT-20261107/`。结果将在本次收尾完成后更新；不把计划当成通过。

## 归档与清理范围

本地审计目录 `.local/cleanup-audits/20261107-closeout/`：

- `worktrees-before.json` / `branches-before.txt`：原路径、提交、合并关系、状态和尺寸。
- `root-pending.json` / `root-pending/`：原根目录文档备份与哈希。
- `plan.json`：固定的 19 个工作树、9 个旧构建、2 个重复 venv、1 个基线源码副本，共 31 项；原占用 **5,732,624 KiB**。
- `closeout_cleanup.py` / `archive.log`：先完整压缩，再校验每个普通文件 SHA-256、目录/文件模式、符号链接目标及源目录未变化。
- 归档保存在 `.local/worktree-archives/20261107-closeout/`，31 份 archive 及同名 manifest；已校验 **60,629** 个条目，压缩文件总计 **507,030,452 字节**。工作树归档仅排除 `.git` 管理指针，源码可由记录提交恢复。

删除阶段只处理归档已验证、源内容未变化、干净且提交已在 main 中的目录；实际收据 `cleanup-receipt.json` 在执行后产生。本次保留 `.local/build-main`、`.local/venv-callback`、全部根目录 results、旧归档、历史材料与环境记录。

## 恢复与后续入口

每份 manifest 提供原路径、HEAD、archive SHA 和条目哈希。恢复工作树先用记录提交创建新的 detached worktree，再在对应目录恢复 archive（归档中无 `.git`）；恢复构建/venv 使用记录的原绝对路径。详细步骤见本地归档目录 README。不得把历史归档直接覆盖现有 main。

后续只从最新 main 开新任务，先读 [当前状态](../CURRENT_STATUS.md)、[支持矩阵](SUPPORT-MATRIX.md) 和 [待办清单](../BACKLOG.md)。本批自动验证不替代物理输入/IME/焦点验收，也不证明通用 EXE、真实应用、独立 `.app` 或性能收益。
