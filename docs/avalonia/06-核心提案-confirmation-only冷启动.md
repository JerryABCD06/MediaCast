# 核心改动提案：confirmation-only 自动冷启动

日期：2026-11-14  
状态：提案，未实施

## 1. 原契约

```text
Protocol
  -> 判断是否需要投送确认
  -> 向 Shell 发确认请求

Shell
  -> 唯一子进程启动者
  -> 选择普通 Ui / confirmation-only Ui
  -> 监督进程和窗口 ready

Ui
  -> 普通模式：主窗口、MPV、PlaybackBridge、SMTC
  -> confirmation-only：只连接 ControlPipe 并显示确认窗口
```

## 2. 当前缺口

confirmation-only Ui 本身已经完成：

```text
不创建 MainWindow
不创建 MpvCore
不连接 PlaybackPipe
不创建 PlaybackBridge
不创建 SMTC
只显示确认窗口并返回决定
```

但 Shell 尚未在“主窗口不存在、当前只需要确认”时启动该模式。
没有现成 ControlPipe 连接时，确认请求只能 fail-closed。

## 3. 为什么 UI 不能单独解决

UI 进程尚未存在时，没有 UI 代码可以接收确认请求。

若由 Protocol 直接启动 UI，会破坏：

```text
Shell 是唯一 Ui 启动者；
Shell 是 Ui 进程状态所有者。
```

若普通 Ui 内再创建第二套独立生命周期，也会形成第二个 Ui owner。

## 4. 建议的所有权

| 状态/动作 | 所有者 |
|---|---|
| 是否需要确认 | Protocol / CastArbiter |
| 选择普通或 confirmation-only Ui | Shell |
| Ui 启动、终止、超时 | Shell |
| 确认请求内容与 claim 校验 | Protocol |
| 确认窗口和用户决定 | Ui |
| owner 迁移 | Protocol |

## 5. 建议流程

```text
Protocol 请求 confirmation-only 显示
  -> Shell 检查现有 Ui
       有普通 Ui：转发给现有 Ui
       有 confirmation-only Ui：转发给现有 Ui
       都没有：启动 confirmation-only Ui
  -> 等待 ControlPipe Hello + 辅助窗口 ready
  -> 转发确认请求
```

辅助窗口 ready 必须是独立中性信号，不能伪装成主窗口 `UiFirstFrame`。
如果现有 ControlPipe 没有合适消息，wire 增量必须另行确认。

## 6. 必须保持

- 已有普通窗口时不额外启动 confirmation-only；
- 同一时刻最多一个 Ui；
- 主窗口 `UiActivateRequest -> UiFirstFrame` 规则不变；
- confirmation-only 不使用主窗口首帧看门狗；
- 拒绝、超时、关闭、断管全部 fail-closed；
- claimId / requestId / protocol 校验不变；
- confirmation-only 不连接 PlaybackPipe、MPV、SMTC；
- Shell 仍是唯一启动者；
- Job Object 和退出清理不变。

## 7. 失败路径

```text
进程启动失败
ControlPipe 连接失败
辅助窗口 ready 超时
请求在 ready 前到达
ready 后转发失败
用户关闭窗口
Shell 退出
ControlPipe 断开
决定发送失败
重复启动请求
确认期间普通 Ui 被激活
```

全部结果默认拒绝或取消，待确认候选不得取得 owner。

## 8. 回归

```text
无普通 Ui + 需要确认
  -> confirmation-only 启动
  -> Hello
  -> 辅助窗口 ready
  -> 请求显示
  -> 允许 / 拒绝 / 取消 / 超时

普通 Ui 已存在
  -> 不启动第二个 Ui
  -> 原 Ui 显示确认

confirmation-only 已存在 + 第二次确认
  -> 不重复启动
  -> owner 规则不变

ready 超时
  -> 取消待确认
  -> owner 不变
  -> Ui 清理
```

同时证明：

- 没有 MPV/SMTC；
- 没有 PlaybackPipe；
- Shell 日志明确记录 Ui 模式；
- 结束后无残留进程。

## 9. 请确认

请批准修改：

```text
src/shell/
必要时 ControlPipe 的中性增量消息
```

未批准前，只保留显式开发验收：

```powershell
MCast.Ui.exe --standalone --confirmation-only --test-takeover-window
```

正常自动冷启动不会被声明为已完成。

