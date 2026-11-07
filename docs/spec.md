# ra9fael.port 功能规格（v0.1.0）

状态：设计完成，代码未实现。协议与清单字段以 WinShun 0.22.0 插件文档为准（resident JSON-RPC over stdio）。

## 1 目标

- 「端口被谁占了」一步到位：查询 → 看进程 → 杀
- 全结构化数据（iphlpapi + 进程 API），零 shell 解析

## 2 非目标

- 不做实时流量监控、不做抓包
- 不做防火墙规则管理（v0.2 候选：一键加屏蔽规则）
- 进程名反查端口放 v0.2

## 3 命令（`port` / `端口`）

- 查询格式：`8080` 或 `:8080`；留空 → 全部 LISTEN（TCP）+ 已绑定（UDP），按端口号升序，`maxRows` 裁剪
- 匹配：本地端口 == N（TCP + UDP，v4 + v6）；`showEstablished` 开启时同时列出 local/remote 任一为 N 的 ESTABLISHED 行（remote 命中的标「→ 对端」）
- 行内容：`TCP 0.0.0.0:8080`（v6 显示 `[::]:8080` 形式）｜进程名 + PID｜状态｜路径（无权限读取时省略）

## 4 动作

- `Enter kill`：`OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess`；成功后结果行消失（下次查询确认）；access denied → 行内提示改用管理员
- `Ctrl+Enter killTree`：Toolhelp32 快照按父进程链收集子进程后依次结束；比 kill 更醒目的确认
- `Shift+Enter copyCmdline`：WMI `Win32_Process.CommandLine` 复制到剪贴板
- `openFolder`：宿主 `open` effect 打开 exe 所在目录
- `killAdmin`：UAC 提权执行 `taskkill /PID <n> /T /F`（独立提权子进程），成败以结果行回传
- PID 4（System 占 135/445 等）：行显示 PID 4 + 状态；无默认 kill，仅 `killAdmin` 可选

## 5 行为细节

- 表快照：每次查询现拉 `GetExtendedTcpTable(TCP_TABLE_OWNER_MODULE_ALL, v2, v4+v6)` + `GetExtendedUdpTable(UDP_TABLE_OWNER_MODULE)`，不缓存
- 路径解析：`QueryFullProcessImageNameW`（`PROCESS_QUERY_LIMITED_INFORMATION`）；拒绝访问 → 只显示名称，不报错
- 同 PID 占多端口 → 保留多行，行内注明「同一进程共 N 个端口」；kill 作用于整个 PID
- kill / killTree 是危险操作：行样式 `valid:false` + 宿主确认流程
- 端口号范围 1–65535，其余 → 提示行
- 纯 Win32 + CRT 的 port.exe，无 Qt 依赖，无 `ui` 权限

## 6 权限

- `processes`：枚举 + 结束
- `clipboard:write`：copyCmdline
- `elevated`：killAdmin 的 UAC 路径
- 不做 `ui`、`network`（openFolder 走宿主 effect，不需 files 权限）

## 7 数据位置

不持久化；`WS_DATA_DIR` 未用。

## 8 失败处理

- 路径读取失败 → 缺省展示，不报错
- `TerminateProcess` 拒绝 → 提示 killAdmin
- 进程已退出（race）→ 提示「已退出」
- 沙箱下 `OpenProcess` 失败面与 winshun.kill 等同（其用 PowerShell `Stop-Process`），行为对齐

## 9 里程碑

- v0.1：TCP/UDP v4/v6、过滤、kill / killTree / copyCmdline / openFolder / killAdmin
- v0.2 候选：进程名反查端口、防火墙一键屏蔽、两次查询 diff（新出现的监听）
