# 更新日志

本插件所有值得注意的变化。格式遵循
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本遵循
[Semantic Versioning](https://semver.org/lang/zh-CN/)。`plugin.json` 里的
`version` 是唯一事实来源；发版 tag 是 `v<version>`。

## [Unreleased]

## [0.1.0] - 2026-11-08

### Added

- 端口占用查询：`port 8080` / `port :8080` 列出监听或连接该端口的进程
  （TCP + UDP，v4 + v6），留空列出全部监听端口；`port java` 按进程名
  反查其占用的端口（大小写不敏感子串）。
- 动作：结束进程（Enter，两段确认）、连同子进程（Ctrl+Enter）、复制
  命令行（Shift+Enter）、复制 PID / 路径 / netstat 形式整行 / 父进程链、
  打开所在目录、在资源管理器中定位、以管理员结束（UAC，走 taskkill）。
- 系统级监听（PID 4 等）不提供默认结束，仅保留管理员路径。
- 定位/打开目录前以 Explorer 令牌做 ACL 检查，普通用户不可读的目录
  改为复制路径并提示，不再弹资源管理器的「拒绝访问」框。
- 设置：`showEstablished`（默认开）、`showSystem`（默认开）、
  `maxRows`（10–200，默认 30）。
