# Security

**English** · [中文](#中文)

## Reporting

Please do not open a public issue for a security problem.

- Email **ultra.ow@gmail.com** with `[changji security]` in the subject, or
- use GitHub's private reporting:
  [Report a vulnerability](https://github.com/changji-ai/ChangJi/security/advisories/new).

Include the version (`changji --version`), the package, and steps to reproduce.
You will get a reply within a week. Please give us time to ship a fix before
publishing details; we will credit you in the release notes unless you prefer
not to be named.

## Supported versions

There is only ever one current release: the `release` build and the newest
`desktop-v*` desktop app. Fixes ship as a new version; older builds are not
patched. The app checks for updates on its own.

## What is in scope

- The engine's HTTP interface (`changji --port`), including the same-site guard
  and the token check, and the worker protocol between machines
- The desktop app, the Windows installer and uninstaller, the macOS `.dmg`,
  the updater, and `install.sh`
- Files the engine writes or reads on your behalf (project directories, model
  downloads, output)
- Vulnerable third-party components that we ship inside a package

## What is not

- What the generative models produce. That is a quality question, not a
  security one.
- The model weights themselves: they are not part of this software and come
  from their own publishers.
- Problems in upstream libraries that we do not ship — report those upstream.

## How the engine is meant to be exposed

By default the engine listens on `127.0.0.1` only. Started with
`--host 0.0.0.0`, every request that reads or changes anything requires the
token from `[peer].token` in the configuration; the startup banner prints the
address with the token already in it. If you put an engine on the public
internet, put it behind HTTPS and keep the token secret — anyone who has it can
run tasks on your GPU and read your projects.

---

## 中文

### 报告

安全问题请**不要**开公开的 issue。

- 发邮件到 **ultra.ow@gmail.com**，标题带上 `[changji security]`；或者
- 用 GitHub 的私下报告：
  [Report a vulnerability](https://github.com/changji-ai/ChangJi/security/advisories/new)。

请附上版本（`changji --version`）、装的哪个包、复现步骤。一周内会回复。
请给我们修复并发版的时间再公开细节；除非你不愿意署名，否则会在发布说明里致谢。

### 支持的版本

只有一个当前版本：`release` 那份构建和最新的 `desktop-v*` 桌面端。修复以新版本
发布，旧版本不打补丁；程序自己会检查更新。

### 范围内

- 引擎的 HTTP 接口（`changji --port`），包括同站守卫和口令检查，以及机器之间的
  工作进程协议
- 桌面端、Windows 安装器和卸载器、macOS `.dmg`、更新器、`install.sh`
- 引擎替你读写的文件（项目目录、模型下载、产物）
- 我们打进包里的、有漏洞的第三方组件

### 范围外

- 生成模型产出的内容——那是质量问题，不是安全问题
- 模型权重本身：它们不属于本软件，来自各自的发布方
- 我们没有打进包里的上游库的问题——请报给上游

### 引擎应该怎么暴露

默认只监听 `127.0.0.1`。用 `--host 0.0.0.0` 启动之后，凡是读或改东西的请求
都要配置里 `[peer].token` 那把口令；启动时终端上打的地址已经带着它。要把引擎
放到公网上，请套上 HTTPS 并保管好口令——拿到口令的人能在你的显卡上跑任务、
读你的片子。
