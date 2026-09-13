# MusicKiller 使用说明书

阻止 **网易云音乐 / QQ音乐** 在 Windows 10 (64位) 上运行的小工具。当 `C:\allow.txt` 存在时放行，不存在时目标进程一启动就被静默关闭。

本仓库提供**两个版本**，按需选用：

| | ⭐ 纯EXE版 (推荐) | 驱动版 (进阶) |
|---|---|---|
| 文件 | `MusicKiller.exe`（单文件） | `MusicKiller.sys` + `mkctl.exe` |
| 原理 | 每秒扫描进程列表，发现目标直接结束 | 内核回调，进程创建瞬间直接拒绝 |
| 无感知 | ✅ 无窗口、无控制台、无提示 | ✅ |
| 需要管理员 | ❌（仅 `allow on`/`install all` 需要） | ✅ |
| 需要测试签名/重启/关Secure Boot | ❌ 全都不需要 | ✅ 都需要 |
| 拦截强度 | 进程启动后 ≤1 秒被杀（可能闪一下窗口） | 进程根本创建不了（更强） |
| 蓝屏风险 | 零（纯用户态） | 极低（仅文档化API） |
| 卸载 | `uninstall` 一键清除 | `mkctl uninstall` |

> ⚠️ 仅供学习与个人自律用途，请勿用于未经授权的计算机。

---

## 一、纯EXE版（推荐）使用说明

### 1. 安装（装一次，永久生效）

把 `MusicKiller.exe` 下载到任意位置，**双击运行一次没有反应是正常的**（它已进入后台静默监控）。推荐使用命令行安装以便看到提示：

```bat
MusicKiller.exe install
```

安装会：复制程序到 `%LOCALAPPDATA%\MusicKiller\` → 注册开机自启（当前用户，**免管理员**）→ 立即在后台启动监控。

所有用户生效（需管理员 cmd）：`MusicKiller.exe install all`

### 2. 验证

```bat
MusicKiller.exe status
```

```
开机自启(当前用户): 已设置 -> "C:\Users\xxx\AppData\Local\MusicKiller\MusicKiller.exe"
监控运行:   正在后台静默运行
allow.txt:  不存在 -> 拦截模式 (拦截网易云/QQ音乐)
```

然后打开网易云音乐/QQ音乐试试——窗口最多闪一下就会被关闭。

### 3. 放行开关

```bat
MusicKiller.exe allow on     :: 创建 C:\allow.txt，放行（需管理员）
MusicKiller.exe allow off    :: 删除 C:\allow.txt，恢复拦截
```

即改即生效，无需重启任何东西。

### 4. 命令一览

| 命令 | 作用 |
|---|---|
| `MusicKiller.exe`（无参数） | 后台静默监控模式（开机自启调用的就是它） |
| `run` | 前台监控（可看实时日志，Ctrl+C 退出） |
| `kill` | 立即清理一次目标进程 |
| `install` / `install all` | 安装开机自启（当前用户 / 所有用户） |
| `uninstall` | 卸载（停止监控 + 删自启 + 删文件） |
| `stop` | 停止后台监控（不卸载，下次登录仍会自启） |
| `status` | 查询自启/运行/放行状态 |
| `allow on\|off\|status` | 放行开关 |

### 5. 日志与卸载

- 运行日志：`%LOCALAPPDATA%\MusicKiller\monitor.log`（记录每次击杀）
- 卸载：`MusicKiller.exe uninstall`

### 6. EXE版已知限制（诚实说明）

- 轮询间隔 1 秒：目标进程**可能闪一下窗口**再被杀（驱动版则完全无法启动）。
- 若目标以**管理员身份**运行而监控程序不是管理员，则无法结束它（极少见）。
- 懂技术的用户可以改名绕过（按进程名前缀匹配）。
- 微软商店 UWP 版（如"QQ音乐 UWP"）进程模型不同，不在拦截范围内。

---

## 二、进程匹配说明（已核实）

| 软件 | 主进程 | 安装位置 | 匹配前缀 |
|---|---|---|---|
| 网易云音乐 PC 版 | `cloudmusic.exe` | `%LOCALAPPDATA%\Netease\CloudMusic\` | `cloudmusic` |
| QQ音乐 PC 版 | `QQMusic.exe` | `C:\Program Files (x86)\Tencent\QQMusic\` | `qqmusic`、`qmbrowser` |

前缀匹配大小写不敏感，主程序与辅助进程（如 `cloudmusic_reporter.exe`、`QQMusicExternal.exe`、`qmbrowser.exe`）全覆盖。

---

## 三、驱动版（进阶，可选）

仅在需要"进程连启动都不可能"的最强拦截时使用。文件：`MusicKiller.sys`（内核驱动，CI 已测试签名）+ `mkctl.exe`（控制器）。

> 原理参考微软官方驱动示例 `Windows-driver-samples/general/obcallback`（`PsSetCreateProcessNotifyRoutineEx` 拒绝进程创建）。

### 快速开始（共 4 步，全程管理员）

```bat
mkctl testsign on     :: 1. 开测试签名（Secure Boot 开启时需先关：BIOS 里 Disabled）
                      :: 2. 重启电脑
mkctl setup           :: 3. 一键安装并启动驱动（自动启动，重启永久生效）
mkctl status          :: 4. 查看状态
```

### 驱动版命令

| 命令 | 作用 |
|---|---|
| `mkctl setup` | 一键安装 + 启动驱动 |
| `mkctl install [sys路径]` | 仅安装驱动服务 |
| `mkctl start` / `stop` / `restart` | 启动 / 停止 / 重启驱动 |
| `mkctl status` | 驱动状态 / 启动类型 / allow.txt / 测试签名 / Secure Boot |
| `mkctl uninstall` | 停止并卸载驱动 |
| `mkctl allow on\|off\|status` | 放行开关 |
| `mkctl testsign on\|off` | 测试签名模式（改后需重启） |

### 驱动版 FAQ

**Q：`mkctl testsign on` 失败 / "被安全启动策略保护"？**
Secure Boot 开着。进 BIOS/UEFI 关闭 Secure Boot 后重试。v1.1 起 mkctl 会回显 bcdedit 原始错误并自动检测 Secure Boot（`mkctl status` 里也显示）。

**Q：`mkctl start` 报错 577？**
测试签名未生效：确认 `mkctl status` 显示"测试签名: 已启用"，没启用就 `mkctl testsign on` → 重启。

**Q：测试模式水印？**
开启测试签名后桌面右下角会有"测试模式"水印，纯外观。个别带反作弊的游戏（Valorant 等）会因此拒绝运行。

**Q：安全吗？会蓝屏吗？**
驱动约 200 行，只用文档化 API，无 hook 无补丁；自动启动服务在系统起来后才加载，加载失败不影响开机；安全模式不会加载它，可进安全模式 `sc delete MusicKiller` 摘除。风险极低，但仍建议先在虚拟机体验。

---

## 四、自行构建（GitHub Actions）

无需本地装 WDK/VS，打 tag 即可，流水线自动编译+签名+发 Release：

```bash
git tag v2.0.0
git push origin v2.0.0
```

## 五、免责声明

本工具用于学习 Windows 编程与**个人自律**（如专注学习时屏蔽娱乐软件）。使用者需获得目标计算机所有者的授权。作者不对任何滥用行为负责。
