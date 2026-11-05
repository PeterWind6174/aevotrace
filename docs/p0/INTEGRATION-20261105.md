# P0 代码合并与本地工作树整理

日期：2026-11-05。状态：按用户要求完成本地 main 合并与已合并工作树移除，未推送。

## 合并范围

- main 原基线：`9d66cea`。
- `e99efc6`：提交并保留此前八个协作约定/agent 配置修改。
- `555156a`：合并 `p0/engine-spike` 的 `b4052dd`，包括执行核心、F1–F5、空 AOT 表和全部回归。
- `179cd12`：合并 `p0/ui-spike` 的 `847dad7`，包括 compat/native GUI 语义实验及 Windows 可复现构建修复。
- `p0/qa` 与 `p0/trace-investigation` 的提交已在原 main 历史内，没有重复合并。

当前代码位置：`spikes/p0-engine/`、`spikes/p0-ui/`。两套源码与各自已验收分支逐项一致。合并没有连接二者的执行接口，也没有改变 P0 未整体通过的结论。

## 合并后验证

在 main 的 `179cd12`，从根目录源码全新构建：

```sh
cmake -S spikes/p0-engine -B .local/build-main-integration-20261105 -G Ninja
cmake --build .local/build-main-integration-20261105
ctest --test-dir .local/build-main-integration-20261105 --output-on-failure
```

configure/build/ctest 均返回 0，ctest 3/3 PASS。UI 的九个 Python 模块完成 AST 语法检查；GUI 运行源码与已验收 `847dad7` 完全一致，本次未重复启动可见窗口或做人工验收。

构建命令和日志保存在下述本地归档目录的 `main-build-commands.json`、`main-build-0.log` 至 `main-build-2.log`。后续文档状态更新不修改实验源码。

## 证据保留

归档位置：`.local/worktree-archives/20261105_195018/`（由 gitignore 排除）。

移除前逐项保存全部未跟踪/忽略文件，包括 QA 截图、日志、输入二进制、构建产物与虚拟环境。归档通过成员数、文件 SHA-256 和符号链接目标校验；删除前再次核对 HEAD、祖先关系、工作树状态和每个原文件哈希。

- `manifest.json`：原工作树 HEAD、分支、文件与归档哈希。
- `<worktree-name>.tar.gz`：该工作树的全部非跟踪材料；成员路径相对于原工作树根目录。
- `root-uncommitted.patch`：提交前的八份协作配置备份。
- `removed-worktrees.json`：逐个移除结果。

例如最终 UI QA 的 `spikes/p0-ui/qa-evidence2/` 在 `p0-qa-ui2.tar.gz` 中；已跟踪源码与报告在 main 和保留的分支历史中。旧报告保留实际执行时的路径，阅读时结合本记录定位。

根目录 `.local/results/` 的原始证据、历次独立审查及本地备份保持原样。

## 移除范围

以下 11 个附加工作树均确认 HEAD 已被 main 包含，无未保存的跟踪文件修改：

- p0-core-fix
- p0-core-fix-02
- p0-core-fix-02-qa
- p0-core-fix-qa
- p0-engine
- p0-qa
- p0-qa-engine
- p0-qa-ui
- p0-qa-ui2
- p0-trace
- p0-ui

仅两个 UI QA 工作树因存在已归档的未跟踪证据而使用 `git worktree remove --force`；其余使用普通移除。保留所有本地分支引用，不执行分支删除或远端推送。当前只保留根 main 工作树；未来任务从 main 新建工作树。
