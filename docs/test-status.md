# Apex 测试状态记录

## 2026-11-25 — Wallpaper 1.2.9

本轮只修改 Wallpaper：显示器身份缓存从“EDID 型号码”改为“完整硬件设备路径 + EDID”，
修复同型号显示器重接或更换后命中旧身份的问题；单台坏路径改为不阻塞其它屏幕；新屏无原图时补用
`Transcoded_*` / `CachedFiles` 缓存合成；正常桌面路径改为原生
`IDesktopWallpaper::SetWallpaper(monitorID, path)`，真实 monitorID 与显示槽位别名分离。
逐屏调用后记录 monitorID、目标路径和 `GetWallpaper` 回读路径，防止误用旧的合成图路径。
锁定检测把插件自己写入的来源/目标路径加入白名单，只恢复外部修改。
合成提交后恢复原有系统显示位置，避免把“适应”改成“跨区”；原位置写入 `Wallpaper.ini` 的
`system_position`，旧配置默认按“适应”恢复。

- `apex/build.sh`：通过
- `check_feature_wallpaper`：通过（ABI 35 / 结构 144；真实 DLL、插件图片副本、缩略图、锁屏和隐藏切换控件）

多屏热插拔和实际换屏后的视觉结果由用户验证。

## 2026-11-23 — Apex 1.13.1 / ABI 35 / SystemStatus 1.5.4

本轮修改宿主、ABI、AutoIME 的 ABI 副本和系统状态；其它功能的行为未改。

- `apex/build.sh`：通过；全部插件按 ABI 35 重新构建
- 构建层：**27 / 27 通过**
- `check_apex_hostconfig`：通过（任务栏显示位在原位置改名，位置和左右侧不变）
- `check_apex_taskbar_display`：通过
- `check_feature_systemstatus`：通过（显卡稳定键、旧开关/单位迁移、任务栏旧键别名）
- `check_apex_modular`：通过（C 与 Rust 两侧均为 `ApexHost=104 ApexFeature=144`，ABI 35）
- 装配层：**33 / 37 通过**；未通过的仍是 `check_apex_icons`、`check_apex_endtoend`、
  `check_apex_persist`、`check_apex_flyout` 四项已知工具会话环境性失败
- 已部署到 `D:\App protable\Apex`
- 部署后只读核对：`SystemStatus.ini` 已写入稳定 GPU 键和 `.legacy` 别名；已启用的显卡项在
  `apex.ini` 的 `taskbarorder` 中已原位改成稳定键，左右侧仍为原来的 `left`

Windows 重启后的实际持久化由用户验证。

## 2026-11-22 — Apex 1.13.0

本轮只修改宿主、任务栏显示层和通用设置面板；其它功能未改，沿用 1.12.18 的通过记录。

- `check_apex_hostconfig`：通过（新设置写入、读取和默认值正确）
- `check_apex_taskbar_display`：通过（全屏检测、200 ms 隐藏轮询和任务栏绘制）
- `check_apex_panel`：通过（「全屏时隐藏」开关渲染并发送设置）
- `apex/build.sh`：通过
- 已部署到 `D:\App protable\Apex`

实际全屏视频和全屏游戏的视觉验证由用户完成。

## 2026-11-22 — Apex 1.12.18 / ABI 34

本次完整验收在 Windows 工具进程树之外运行，避免当前工具会话的 `0xc0000142` /
`ERROR_DLL_INIT_FAILED` 环境干扰。

### 构建层

**27 / 27 通过。**

- 宿主边界、语言、配置、调色板、模块化、文档长度、共享模型同步全部通过。
- AutoIME：44 个 Rust 测试全部通过，并成功生成 `AutoIME.dll`。
- Compute Manager、Wallpaper、System Status、Volume Guard、Media Control 的真实 DLL 探针全部通过。
- 任务栏统一排版、面板 DOM、部署脚本、便携包白名单全部通过。
- 便携包确认包含：AutoIME、KeepAwake、MediaControl、SmoothWheel、VolumeLock、
  ComputeManager、Wallpaper、SystemStatus。

### 装配层

**33 / 37 通过。**

已通过：

- `check_feature_edit`
- `check_reaper_note`
- `check_reaper_live`
- `check_apex_shutdown`
- `check_apex_flash`
- `check_apex_single`

其余装配门也通过；下方四项失败是项目既有文档记录的本机环境性红：

| 门 | 现象 | 结论 |
|---|---|---|
| `check_apex_icons` | 工具会话无法让 `Shell_NotifyIcon` 完成托盘状态探测 | 非代码回归；用户运行日志中托盘正常 |
| `check_apex_endtoend` | 工具会话收不到注入滚轮事件 | 非代码回归；输入会话隔离 |
| `check_apex_persist` | 强制终止后的探测进程未完成落盘观察 | 非代码回归；构建层配置门通过 |
| `check_apex_flyout` | 工具会话中的分层窗口预览不完整 | 非代码回归；快速面板绘制门通过 |

这四扇门在 `docs/rules/gates.md` 的“已知的环境性红”中已有同样记录。

### 本轮额外验证

- `SystemStatus.dll` 编译产物中不含 `ApexSystemStatusTaskbar` / `PaintTaskbar` 旧符号。
- Compute Manager 的旧任务栏窗口、几何、遮挡扫描和自绘代码已移除。
- Volume Guard 源码、探针、门和发布包均已公开，不再受 `.gitignore` 限制。
- `git diff --check` 无空白错误。

## 以后怎么用这份记录

1. 下一批改动前，先看本文件和 `git diff --name-only`。
2. 只改了某个功能目录，就只跑该功能的构建门；其它模块保持上一次通过状态。
3. 改了宿主、ABI、公共头、面板、打包脚本或多个模块时，扩大到对应宿主/装配门。
4. 任何模块只要源码或依赖发生变化，就把它的状态视为“待验证”，不得继续沿用旧结论。
5. 新结果覆盖本节顶部日期和版本；旧版本无需长期堆积。
