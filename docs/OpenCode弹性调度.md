# OpenCode 按任务增加会话的约定

日期：2026-11-06。用户已授权需要时增加 agent 或由主 agent 开更多会话。此约定是项目调度规则，不修改 OpenCode 全局设置、模型、账号或订阅。

## 角色与会话

当前六个角色：coordinator、compiler、binary、platform、qa、reviewer，覆盖现阶段职责。一个角色可以承接多个独立会话，例如 qa-behavior、qa-artifact、qa-windows；这些是任务名，无需为每个任务复制角色模板。新增会话是否可用、并发额度和模型设置须以实际桌面端验证为准。

主控负责创建、分派与整合全部子任务；worker 不继续派生，已有 subagent deny 权限不放开。没有独立任务时不增加会话，小修复只需一个实施者、独立 QA 和 reviewer。

## 并发条件

- 常态最多两个实施任务同时写代码。
- 主控可以在不另问用户的情况下扩到最多四个实施任务，但必须先有已明确的契约和文件 owner、独立 worktree/branch/build/cache，且 CPU/内存、磁盘、Windows 测试目录和桌面能力允许。
- ABI、artifact schema、dispatcher、公共构建文件等共享范围始终单 owner；依赖接口尚未确认时顺序实施。worktree 隔离不能消除逻辑冲突。
- 独立 QA、只读调查/reviewer 可按需增加，以任务价值和资源为限，不预设无限并发。指定同一最终源码提交，各自使用独立结果/构建目录；GUI 与共享 Windows 参考资源需要预约或串行。
- 每个任务记录名称、角色、依赖、基线、文件范围、工作树/分支/构建/缓存/结果目录、验收和实际模型设置。只派发任务提示文字不等于完成目录隔离。

## 模型与工具

所有主/新/子会话实际配置均须核对 OpenCode Go / DeepSeek V4.1 Flash / Max，不假定继承。只使用桌面端原生会话/子 agent；不调用 opencode run/CLI 模型 worker，不擅改账号或密钥。桌面隔离/模型/资源条件不满足时减少并发或串行，保留能独立完成的工作。

## 何时增加角色模板

只有出现现有角色职责覆盖不了的长期独立范围时增加模板，例如打包阶段需要专门的构建/分发 owner。新增前写清职责、权限、目录 ownership、交接与验收，沿用已验证的配置结构；不得因此修改用户 provider/model。临时验证不同边界优先增开现有 QA/reviewer 会话。

## 本次修复的建议分工

P0-PROMOTION-FIX-01：一个 compiler 实施者；binary 会签实际 artifact 身份/基线语义；QA 独立验证全 AOT 再补充与 B-only→A 两条回归；reviewer 检查差集语义及 publisher/GUI 消费者。QA 可分行为与产物两个独立会话，但应在实现提交冻结后验证，不与实施者共改源码。本次不需要四个实施者或新增角色。
