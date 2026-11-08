# ra9fael.port — Win顺 端口占用查询插件

`port 8080` 一步查出占用端口的进程并支持结束；纯 iphlpapi 结构化数据，不解析 netsh。与 winshun.kill（按名字/PID 杀）互补，补上「端口」维度。

状态：**v0.1 已实现**（纯 Win32 + CRT 的 resident 插件，单测 101 项全绿；协议往返 E2E 与系统套接字列表交叉核对通过）。

## 使用

- `port 8080` / `port :8080`：列出监听或连接 8080 的进程（TCP + UDP，v4 + v6）
- `port java`：按进程名反查它占用的全部端口（大小写不敏感子串）
- 留空：列出全部监听端口（按端口升序，`maxRows` 截断）
- Enter：结束进程（危险操作，需按两次确认）；Ctrl+Enter：连同子进程；Shift+Enter：复制命令行
- 动作菜单：复制 PID / 路径 / netstat 形式整行 / 父进程链，打开所在目录，在资源管理器中定位，以管理员结束（UAC）
- 系统级监听（PID 4 等）无默认结束，仅提供管理员路径

## 开发

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -C Release
```

## 文档

- 用户指南：[docs/guide.md](docs/guide.md)（安装、查询语法、动作、FAQ）
- 开发文档：[docs/develop.md](docs/develop.md)（架构、协议、数据来源、已知的坑、扩展指南）
- 功能规格：[docs/spec.md](docs/spec.md)
- 测试规格：[docs/tests.md](docs/tests.md)
- 发布与安装：[docs/release.md](docs/release.md)

English code/comments per project convention; user-facing strings follow the host locale (zh/en).

## License

MIT — see [LICENSE](LICENSE).
