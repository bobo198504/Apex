# 暗色标题栏：实测结论与做法（Windows 桌面窗口）

> 2026-09-18 立。起因：Apex 设置面板的 WebView2 内容已经是深色，**标题栏还是浅的**。
> 这是**标题栏（非客户区）**的问题，与页面无关——页面再深也管不到系统画的那条。

## 一、结论

**能变深，而且只需要一次调用**：`DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ...)`。

### 实测数据（`_diag/caption_brightness.ps1`，按 Rec.601 亮度采样标题栏条带）

| 版本 | 标题栏平均亮度 | 判定 |
|---|---|---|
| **不调** `DwmSetWindowAttribute` | **243.0** | 浅色 |
| **调用**了（本机 Win11 10.0.28000） | **32.0** | **深色** |

两次测量都是**在窗口被移到副屏、没有焦点**的状态下取的，结论一致 → 该属性与焦点无关，
之前"看起来没变"是**看错了**（未激活窗口的标题栏本身就比激活态灰，容易被当成"浅色"）。

## 二、做法（照抄插件里的 `ApplyTitleBar`）

```cpp
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19   // Windows 10 2004 之前
#define DWMWA_USE_IMMERSIVE_DARK_MODE     20   // Windows 10 2004 及以后
#define DWMWA_BORDER_COLOR                34   // Windows 11：不设它，深色标题栏外面会留一圈亮边

BOOL v = dark ? TRUE : FALSE;
if (FAILED(fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v))))
  fn(h, DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &v, sizeof(v));   // 老系统回退
fn(h, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor)); // 可选，Win11 才认
```

要点：

1. **两向开关**：同一个调用传 `FALSE` 就变回浅色，所以是"跟随系统"而不是"强制变深"。
   系统切明暗时要**重新调用**（`WM_SETTINGCHANGE`，见下）。
2. **`dwmapi.dll` 按需加载**（`LoadLibrary` + `GetProcAddress`），**不要静态链接**：
   属性号随 Windows 版本变，而且缺了它只是标题栏颜色不对，不该让程序起不来。
3. **在首次绘制之前调用**，否则会先闪一下浅色标题栏。
4. 属性调用**失败也无所谓**（老系统），标题栏保持系统色即可——纯装饰，不是功能。

## 三、⚠️ 我在此过程中犯的错（记下来）

1. **断言"没有 manifest 就不生效"**：我看到标题栏没变，就推断是 manifest 的问题并写进了结论。
   实测（加了 manifest 的版本 vs 不加的版本，亮度都是 32）**推翻了这个判断**。
   **教训：把"看起来没生效"当成结论之前，先做 A/B 测量。**
2. **信了截图**：`CopyFromScreen` 抓的是 shell 认为的"主窗口"，窗口被移到副屏时抓到的根本不是被测窗口，
   于是"看"到一张没有变化（或变化无关）的图。**改成按进程枚举窗口 + `GetPixel` 采样亮度**才拿到真数据。
3. **`FindWindowW` 按类名找不到自己的窗口**（但能找到 `Shell_TrayWnd`，说明 API 本身没问题）——
   原因未查明，**不必查明**：改成**按 PID 枚举顶层窗口再筛类名**，绕开了这个坑且更稳。
4. **PowerShell 的 `$Pid` 是自动变量**（当前进程 ID），`param([int]$Pid)` 会在脚本跑起来之前就报错。
   参数名改成 `$TargetPid`。

## 四、怎么复测

```bash
# 1. 启动被测程序（用 cmd start 分离，否则进程随命令结束被回收）
cmd //c start "" build/spike.exe
# 2. 取 PID，测标题栏亮度
PID=$(tasklist //FI "IMAGENAME eq spike.exe" //FO CSV //NH | head -1 | cut -d',' -f2 | tr -d '"')
powershell -NoProfile -ExecutionPolicy Bypass -File _diag/caption_brightness.ps1 "$PID"
# 输出 RESULT=DARK / LIGHT / MID
```

**测前先清 `WebView2` 用户数据目录**，否则残留的 WebView2 进程会让新实例的控制器创建失败
（`0x8007139f`，状态无效，与本主题无关但会干扰观察）。

---

# 附：图标与托盘的明暗自适应（2026-09-18，用户要求）

## 一、规则：**底板跟着背景走**，反的是中间的记号

用户原话：「托盘图标和应用图标也要明暗自适配。」

**图标是「底板 + 记号」两件套。底板与它所在的那条栏同色，对比由记号承担。**

| 文件 | 是什么 | 用在哪 |
|---|---|---|
| `apex-light.ico` | **米色底板** `#F4EAC6` + 近黑记号 | **浅色外观** |
| `apex-dark.ico` | **近黑板底** `#10130C` + 米色记号 | **深色外观** |

映射是**顺的**：浅色外观 → `IDI_APEX_LIGHT`，深色外观 → `IDI_APEX_DARK`。

## 一之二、⚠️ 这里原先写的是反的（**留作教训，别看错**）

原文写的是「**浅色 → `apex-dark.ico`**，理由是浅底上要深色才看得见」。**那条推理是错的**，
用户后来直接报了「亮暗的图标反了」。

**错在哪**：「浅底要深图标」对**光秃秃的记号**成立，而这两个图标**带底板**。
底板必须跟着背景走，否则等于在任务栏上贴一个不匹配的亮块/黑方块。

**更值得记的是：我量了像素反而更自信。** 均值量到的正是**底板**，
于是"米色那张更亮"被我读成"它该用在深色桌面上"——**测量没救一个错的前提，只是让它看起来有据可依。**
映射关系要**看图**确认；像素只适合判断"两张是不是同一张""换过去了没有"。

**判断依据是 `AppsUseLightTheme`**（`HKCU\...\Themes\Personalize`）。
但**钉住的 light/dark 盖过它**，统一走 `ThemeResolvesLight(theme, systemIsLight)`（`apex\hostconfig.h`）——
面板、标题栏、托盘、窗口图标四处都问它，才不会互相矛盾。

## 二、四处都要跟着变，缺一不可

| 位置 | 机制 | 坑 |
|---|---|---|
| **面板页面** | CSS `prefers-color-scheme` | 无（浏览器自己处理） |
| **窗口标题栏** | `DwmSetWindowAttribute` | 见上（需要主动调用） |
| **托盘图标** | `LoadImageW` + `Shell_NotifyIconW(NIM_MODIFY)` | 旧图标要在 shell 收下新的之后再 `DestroyIcon` |
| **窗口图标** | `WM_SETICON` | **类的图标是缓存的**，改主题后必须重新 `WM_SETICON`，不会自动跟随 |

**用哪个主题：`ThemeResolvesLight(theme, systemIsLight)`（`apex\hostconfig.h`，纯函数）。**
钉住的 light/dark 盖过系统，`auto` 才看系统。

## 三、⚠️ 本节的旧结论是错的，留着当教训

**这一节原本写的是「实测：切主题时三处都跟着变」，下面是它当时引的证据：**

```
tray: lang=zh icon=light-mark (dark theme) tip="运行中"     ← 系统深色
tray: lang=en icon=light-mark (dark theme) tip="running"    ← 面板改成英文，图标不变
tray: lang=en icon=dark-mark  (light theme) tip="running"   ← 系统切浅色，图标跟着换
tray: lang=en icon=light-mark (dark theme) tip="running"    ← 切回深色
```

**这段日志证明不了它在证明的事。** 那行 `icon=` 打的是**代码打算用哪个**，不是**有没有加载到**。
实际情况是：`apex.rc` 把图稿登记成了**字符串名**，代码按**编号**去要 → `LoadImage` 返回 `NULL` →
shell 从来没收到过新图标。**日志一路说 ok，图标从头到尾没变过。**

**教训有两条，都很便宜就能防住：**
1. **日志要记"结果"，不要记"意图"。** 现在的行是 `icon=.. (.. theme, ..) load=ok`，
   而且 id 与加载结果出自同一次调用。
2. **"两边都跟着变"这种话，要能在产物上验。** 现在 `test/check_apex_icons.sh` 从 exe 里把两张图按编号
   取出来量像素；`_diag/apex_theme_switch_probe.sh` 与 `apex_panel_icon_probe.sh` 真翻转系统主题，
   再**从窗口上读回图标**来量。

**托盘日志仍然要保留**：它是"托盘确实变了"与"托盘没变"之间唯一可读的证据。
但**光有它不够**——它只说意图，不说结果。这也是为什么它旁边现在多了 `load=`。（2026-09-18 修，见 AGENTS.md §3.7。）

## 四、为什么不读托盘来自动断言

试过（`_diag/apex_tray_state.cpp`）：走 `Shell_TrayWnd → TrayNotifyWnd → SysPager → ToolbarWindow32` 那条路。
**本机 `FindWindow("Shell_TrayWnd")` 返回 0** —— Win11 的托盘结构已经和文档描述的不同。
**放弃这条路**（和之前 `thread_owner` 一样），改成验证**规则**：
`test/check_apex_language.sh` 断言语言解析与 UTF-8→UTF-16 转换（乱码在这里会被抓出来），
像素由人眼看。**不为了"自动"而造一个跑不起来的探针。**

## 五、必须用 W 系列 API（实测数据）

托盘/菜单的文字走 `Shell_NotifyIconW` / `AppendMenuW`。
实测本机**走 ANSI 路径，"端"只有 2 字节**（正确的 UTF-8 是 3 字节）——就是乱码的来源。
`test/check_apex_language.sh` 把这条例化成了断言。

（另：`TrackPopupMenu` **没有 A/W 变体**，user32 只导出无后缀的那个——因为 Win32 菜单内部就是 UTF-16 存储，
`AppendMenuW` 写进去、`TrackPopupMenu` 画出来，本来就一致。这不算"用错 API"。）
