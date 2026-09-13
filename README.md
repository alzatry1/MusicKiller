# MusicKiller 使用说明书

Windows 10 64位 内核驱动工具：**拦截网易云音乐 / QQ音乐进程运行**。当 `C:\allow.txt` 存在时放行，不存在时目标进程无法启动、已运行的会被强制结束。

> 实现参考自微软官方驱动示例 `Windows-driver-samples/general/obcallback`（基于 `PsSetCreateProcessNotifyRoutineEx` 拒绝进程创建）。
>
> ⚠️ 仅供学习与个人自律用途，请勿用于未经授权的计算机。

---

## 1. 文件清单

| 文件 | 说明 |
|---|---|
| `MusicKiller.sys` | 内核驱动（CI 编译 + 测试签名） |
| `mkctl.exe` | 驱动注入/控制程序（一键安装、状态查询、放行开关） |
| `使用说明书.md` | 本文档 |

## 2. 工作原理

- 驱动加载后向系统注册**进程创建回调**。任何新进程启动时，驱动检查其镜像文件名（按**前缀**匹配，大小写不敏感）：
  - 命中 `cloudmusic*`（网易云音乐）/ `qqmusic*`（QQ音乐）/ `qmbrowser*`（QQ音乐内置浏览器）：
    - `C:\allow.txt` **存在** → 放行，正常启动；
    - `C:\allow.txt` **不存在** → 拒绝创建，系统提示“拒绝访问 / 无法启动”。
- 启动驱动时，控制器会**强制结束**仍在运行的目标进程（驱动随系统自动启动，早于用户登录加载，重启后目标程序从一开始就无法启动）。
- 服务以**自动启动**方式安装：**安装一次，重启电脑后依然生效**，无需重复操作。
- 修改 `allow.txt` 无需重启驱动，下一次启动目标进程时即按新状态判断。

### 进程信息（已核实）

| 软件 | 主进程 | 安装位置 | 已知辅助进程 |
|---|---|---|---|
| 网易云音乐 PC 版 | `cloudmusic.exe` | `%LOCALAPPDATA%\Netease\CloudMusic\` | `cloudmusic_reporter.exe` 等（前缀 `cloudmusic` 全覆盖） |
| QQ音乐 PC 版 | `QQMusic.exe` | `C:\Program Files (x86)\Tencent\QQMusic\` | `QQMusicExternal.exe`、`qmbrowser.exe` 等（前缀 `qqmusic` / `qmbrowser` 全覆盖） |

> 注：针对 Windows 桌面版（Win32）客户端。微软商店的 UWP 版本（如“QQ音乐 UWP”）进程模型不同，不在本驱动拦截范围内。

## 3. 快速开始（共 4 步）

> 全程需要**管理员权限**。`mkctl.exe` 已声明需要管理员，双击会弹 UAC；建议在“管理员命令提示符 / PowerShell”中操作。

把 `mkctl.exe` 和 `MusicKiller.sys` 放在**同一目录**，然后：

```bat
:: 第 1 步：开启测试签名模式（自签名驱动需要，只需设置一次）
mkctl testsign on

:: 第 2 步：重启电脑（必须，让测试签名模式生效）

:: 第 3 步：重启回来后，一键安装并启动驱动
mkctl setup

:: 第 4 步：验证
mkctl status
```

`mkctl status` 显示如下即为成功：

```
驱动服务:   已安装, 状态 = 运行中 (RUNNING)
启动类型:   自动启动 (重启后自动加载)
驱动文件:   C:\Windows\System32\drivers\MusicKiller.sys (存在)
allow.txt:  不存在 -> 拦截模式 (拦截网易云/QQ音乐)
测试签名:   已启用
```

## 4. 验证拦截效果

1. 确认处于拦截模式（`C:\allow.txt` 不存在，默认即如此）；
2. 双击打开网易云音乐或 QQ音乐；
3. **预期结果**：进程无法启动（系统提示“拒绝访问”或程序一闪而过）；如果软件原本就在运行，驱动启动时会被直接结束。
4. 创建放行文件后再次打开，软件正常启动：

```bat
mkctl allow on    :: 创建 C:\allow.txt，放行
mkctl allow off   :: 删除 C:\allow.txt，恢复拦截
```

## 5. 命令参考

| 命令 | 作用 |
|---|---|
| `mkctl setup` | 一键安装 + 启动驱动（含测试签名检查） |
| `mkctl install [sys路径]` | 仅安装驱动服务（自动启动）。默认复制同目录的 `MusicKiller.sys` 到 `C:\Windows\System32\drivers\` |
| `mkctl start` | 启动驱动 |
| `mkctl stop` | 停止驱动（临时解除拦截） |
| `mkctl restart` | 重启驱动 |
| `mkctl status` | 查询：驱动状态 / 启动类型 / allow.txt / 测试签名 |
| `mkctl uninstall` | 停止并卸载驱动（删除服务与驱动文件） |
| `mkctl allow on\|off\|status` | 创建 / 删除 / 查询 `C:\allow.txt` |
| `mkctl testsign on\|off` | 开启 / 关闭测试签名模式（改后需重启） |

## 6. 卸载

```bat
mkctl uninstall
```

会停止驱动、删除服务（重启后不再加载）、删除 `C:\Windows\System32\drivers\MusicKiller.sys`。如需恢复原样，可再执行 `mkctl testsign off` 并重启关闭测试签名模式。

## 7. 常见问题（FAQ）

**Q1：`mkctl start` 报错 577 / “驱动签名验证失败”？**
测试签名模式未开启或未重启。执行 `mkctl testsign on` → 重启 → `mkctl start`。确认 `mkctl status` 里“测试签名: 已启用”。

**Q2：`mkctl testsign on` 失败 / 提示“被安全启动策略保护”？**
电脑开启了 Secure Boot（安全启动），它禁止开启测试签名。进入 BIOS/UEFI 将 Secure Boot 设为 Disabled，再重新执行命令。v1.1 起 mkctl 会直接显示 bcdedit 的原始错误输出并自动检测 Secure Boot 状态（`mkctl status` 中也能看到）。也可按 Win+R 运行 `msinfo32`，查看“安全启动状态”确认。

**Q3：为什么不用官方签名？**
商业代码签名证书需向 CA 付费申请并经过微软 attestation 签名，个人学习项目一般用测试签名即可。本仓库 CI 每次构建自动生成自签名测试证书并签名。

**Q4：拦截后双击软件弹“拒绝访问”窗口，正常吗？**
正常。驱动在进程创建阶段直接拒绝，这是系统对该错误的提示。软件不会真正启动。

**Q5：软件改名/换路径能绕过吗？**
按**镜像文件名前缀**匹配（`cloudmusic*` / `qqmusic*` / `qmbrowser*`，大小写不敏感），与安装路径无关；改文件名确实可绕过。如需增加/修改目标，编辑 `driver/driver.c` 中的 `g_TargetPrefixes` 数组，重新编译即可。

**Q6：驱动会影响系统稳定性吗？**
驱动只在进程创建回调中做文件名比较与（命中时）一次文件存在性检查，无 Hook、无补丁、不修改任何系统或其他进程内存，可随时 `mkctl stop` 卸载停止。

**Q7：360/电脑管家等安全软件报毒？**
内核驱动 + 进程拦截行为容易被启发式查杀误报。请将两个文件加入信任区，或在虚拟机/自律专用机上使用。

## 8. 自行构建（GitHub Actions）

本仓库自带 CI（`.github/workflows/build.yml`），无需本地安装 WDK：

```bash
git tag v1.0.0
git push origin v1.0.0
```

流水线会在 `windows-2022` 上安装 WDK 10.0.22621 → MSBuild 编译驱动与加载器 → 自签名测试证书签名 `.sys` → 上传构建产物，并按 tag 自动创建 GitHub Release 附上 `MusicKiller.sys`、`mkctl.exe` 与说明书。

## 9. 免责声明

本工具用于学习 Windows 驱动开发与个人自律（如专注学习时屏蔽娱乐软件）。使用者需获得目标计算机所有者的授权。作者不对任何滥用行为负责。
