# 2026-11-07：本批统一合并与收尾

状态：本批代码、文档、主工作树验证和归档清理全部完成，当前开发入口统一为 main。此处只收口已分配工作，不宣布 P0 总体验收或首版产品完成，下一阶段尚未启动。

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

最终 GUI 验证使用 `.local/venv-callback/bin/python`，在干净 main 提交 `034eef441463498b9e899fb915bc5a0b8837b857` 顺序运行；该提交只有文档收口，产品源码与 `47b2b7a` 一致。全部输入、库、AOT 和构建使用根目录路径，结果保存在 `.local/results/CLOSEOUT-20261107/`：

| 项目 | main 实测 |
| --- | --- |
| PE base compat/native | 560/561，load_count=1，calls_total=323，HostImport=105；JIT compile/exec=21/2534，AOT=0 |
| 实际 GUI Trace→离线补充 | selected/promoted=21，demoted=0；使用本次实际 empty AOT 和两份实际 GUI Trace |
| PE supplemented 新进程 | 596/597，load_count=1，AOT=2534，JIT compile/exec=0/0 |
| COFF base/supp/empty × 两后端 | 六组各 44/44，共 264；artifact 全由本次 `.local/build-main` 产生 |
| native 强制失败 | 预期非零退出、无 compat 回退；产物失配/伪 promotion 负例保持拒绝 |
| 旧目录清理后 | 规范 runner 实际执行 PE Reset+Compute，两个 call.status=0，HostImport=1；Ninja dry-run 无需重建 |

`run_validation.py` 为本批验证驱动；`commands.jsonl` 记录完整参数、环境、退出码与耗时；`manifest.json` 绑定源码、二进制/输入/summary 哈希，`logs/` 与 `pe/`/`coff/` 保存日志、Trace 和截图。驱动及 7 个步骤均退出 0。Windows 参考本次未重跑，固定 DLL/helper 未变，复用哈希绑定参考；人工物理 GUI 仍未验收。

## 归档与清理范围

本地审计目录 `.local/cleanup-audits/20261107-closeout/`：

- `worktrees-before.json` / `branches-before.txt`：原路径、提交、合并关系、状态和尺寸。
- `root-pending.json` / `root-pending/`：原根目录文档备份与哈希。
- `plan.json`：固定的 19 个工作树、9 个旧构建、2 个重复 venv、1 个基线源码副本，共 31 项；原占用 **5,732,624 KiB**。
- `closeout_cleanup.py` / `archive.log`：先完整压缩，再校验每个普通文件 SHA-256、目录/文件模式、符号链接目标及源目录未变化。
- 归档保存在 `.local/worktree-archives/20261107-closeout/`，31 份 archive 及同名 manifest；已校验 **60,629** 个条目，压缩文件总计 **507,030,452 字节**。工作树归档仅排除 `.git` 管理指针，源码可由记录提交恢复。

删除阶段已完成：仅处理归档已验证、源内容未变化、干净且提交已在 main 中的目录。`cleanup-receipt.json` 记录全部 31 项的移除与归档哈希；19 个附加工作树、9 个旧 build、2 个重复 venv、1 份基线源码副本已移除。25 个已合并本地任务分支已删除，原名称/提交见 `branches-removed.json`，清理前全部引用另存为已校验的 `repository-before-cleanup.bundle`。当前只保留 main 工作树和本地 main 分支；临时文档 stash 已确认材料保留后清除。

本次保留 `.local/build-main`、`.local/venv-callback`、全部根目录原始测试证据、旧归档、历史材料与环境记录。对原 results/build-archives/history 快照的复核确认 **14,162 份文件哈希不变**；另 1 个 `.local/results/.DS_Store` 元数据发生变化，已单独记录差异，测试证据无损失。空 `.worktrees` 父目录内的 Finder 元数据先备份后移除。

同口径项目根目录 `du -sk`：清理前 **7,061,144 KiB（约 6.73 GiB）**，清理后 **2,074,980 KiB（约 1.98 GiB）**，净减少约 **4.75 GiB**；结果包含新 main 构建及保留的归档和证据。此为目录占用口径，不作为整个 APFS 卷空闲空间变化归因。

`git fsck --full --no-reflogs` 最终退出 0。检查中发现 `.git/refs/.DS_Store` 被 Git 当作非法引用；验证为 Bud1 Finder 元数据、备份后仅移除该文件，未重建仓库或裁剪悬空对象。相关记录为 `git-metadata-cleanup.json` 与 `git-fsck-final.log`。

## 恢复与后续入口

每份 manifest 提供原路径、HEAD、archive SHA 和条目哈希。恢复工作树先用记录提交创建新的 detached worktree，再在对应目录恢复 archive（归档中无 `.git`）；恢复构建/venv 使用记录的原绝对路径。详细步骤见本地归档目录 README。不得把历史归档直接覆盖现有 main。

后续只从最新 main 开新任务，先读 [当前状态](../CURRENT_STATUS.md)、[支持矩阵](SUPPORT-MATRIX.md) 和 [待办清单](../BACKLOG.md)。本批自动验证不替代物理输入/IME/焦点验收，也不证明通用 EXE、真实应用、独立 `.app` 或性能收益。
