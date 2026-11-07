# ra9fael.port 测试规格

## 1 单元测试（纯逻辑，`tests/test_port.exe`，ctest 驱动）

- 查询解析：`8080`、`:8080`、空、`0`、`65536`、`abc`、带空格 → 各自的正确分支
- 表行 → 条目映射：v4/v6 地址格式化（含 `[::]` 与 IPv4-mapped）、状态映射、UDP 无状态标注
- 同 PID 多行聚合标注「同一进程共 N 个端口」
- 排序：留空时端口号升序；协议分组顺序稳定
- killTree 子进程收集：`tools/TestTree`（父→子→孙三层进程树）用快照注入在逻辑层验证
- 设置解析：`maxRows` 10–200 裁剪、两个 bool 开关
- 目标：≥30 用例

## 2 协议往返（真实进程）

- `initialize → query → action → shutdown` 全链路 JSON-RPC
- 空查询 / 命中 / 未命中端口
- action kill / killTree 的 effects 与结果刷新
- `timeoutMs` 1500：满表（数百监听条目）下仍及时响应

## 3 手动 E2E（Windows 11，宿主 0.22.0+）

- [ ] `python -m http.server 8123` 起监听 → `port 8123` 命中 python.exe
- [ ] Enter 杀掉 → 重查询无该行、进程消失
- [ ] tools/TestTree 三层进程树 → Ctrl+Enter 一并结束（核对子进程全灭）
- [ ] Shift+Enter 命令行与实际一致
- [ ] openFolder 打开 python.exe 所在目录
- [ ] `port` 留空：前 20 行与 `netstat -ano` 交叉核对一致
- [ ] `port 135` → PID 4 行无默认 kill，killAdmin 走 UAC；拒绝 UAC → 明确报错
- [ ] `showSystem=false` → PID 4 行隐藏
- [ ] 开一条长连接（curl 挂着）→ ESTABLISHED 行显示对端
- [ ] 中英文 locale

## 4 验收标准

- 单测全绿；协议往返通过；E2E 全勾
- 与 netstat 交叉一致；杀进程操作全部经确认流程，无静默误杀
