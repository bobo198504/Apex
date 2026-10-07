# Apex

> 端，体之无序而最前者也。 —— 《墨经》
>
> **Apex. The first point.**

*[English](README.en.md)*

**端** —— 物体分割到不能再分时，最靠前的那个点。最小的单位，也是最初的起点。

---

## 这是什么

一个 Windows 工具集（类似 PowerToys 的形态）：每个功能只做一件事。

| 功能 | 做什么 |
|---|---|
| **丝滑滚动** | 全局接管鼠标滚轮——任何程序里滚动都带加速和收尾 |
| **自动输入法** | 按当前焦点所在的程序或控件，自动切换中英文输入法；规则可拖动排序、点一下再按键录制 |
| **保持唤醒** | 一张程序列表，每行两个开关——「保持唤醒」和「阻止熄屏」；另有一行不可删的「系统全局」 |
| **媒体控制** | 每台显示器一个亮度推子、一个熄屏按钮；每个应用一个音量推子和一个静音按钮 |

每个功能**各管各的**：各有自己的开关、设置文件和「排除」名单。

## 与两个自带引擎的项目

这套平滑引擎**对一些应用做了单独内嵌**，Apex 对它们让路——**不管对方的引擎有没有打开**：

| 项目 | 是什么 |
|---|---|
| **[SmoothWheelScroll](https://github.com/bobo198504/SmoothWheelScroll)** | REAPER 的平滑滚动插件 |
| **[Lertaro](https://github.com/bobo198504/Lertaro)** | 独立的 Windows 工具（**fork 版**），移植了同一套平滑模型 |

## 用法

1. 运行 `apex.exe`，托盘出现图标
2. **单击**托盘图标 → **快速面板**（浮在图标上方，放各功能的开关、推子、按钮）
3. **双击**托盘图标，或右键 → **设置**，打开设置面板
4. 要让某个程序不受**某个功能**影响，在那个**功能页的「排除」**里加上它的名字（支持 `*` 和 `?`）

## 便携

整个文件夹拷走即可，设置跟着走。不写注册表、不写 `%APPDATA%`。

```
apex.exe              主程序
apex.ini              主设置
apex-settings.exe     设置面板
Plugins\<功能名>\      各功能，含它自己的设置
```

「开机启动」默认关闭（它是唯一写在自身文件夹之外的东西）。首次打开设置面板会生成 `WebView2\` 缓存目录——本机产物，不要拷给别人。

## 构建

```bash
bash apex/build.sh                # 构建，产物在 build/apex/
bash test/run_all.sh              # 构建层门：纯逻辑与产物，不起进程、不碰输入设备
bash test/run_all.sh --assembly   # 再加装配层：会起真程序，交付前跑一次
bash apex/package.sh              # 便携包 build/Apex-<版本>-portable.zip
bash apex/deploy.sh               # 部署到本机安装目录
```

需要 **MinGW-w64**（`g++` / `windres`）；项目用 [w64devkit](https://github.com/skeeto/w64devkit) 开发，
脚本顶部有一行默认工具链路径。不需要 REAPER SDK、不需要 CMake；WebView2 SDK 随项目附带（`third_party/`）。

## 开发

结构与约定见 **[AGENTS.md](AGENTS.md)**（按主题的分册在 `docs/rules/`）；每一版改了什么见 **[CHANGELOG.md](CHANGELOG.md)**。

- **功能 = 一个文件夹**（`features\<id>\`），实现 `apex/abi.h` 的 C 接口；界面不用为每个功能写
- **设置与钩子是两个进程**：面板卡死不影响滚动
- **模型在 `shared/`**，是 SmoothWheelScroll 插件 `src/` 的副本，两边手动同步（`test/check_model_sync.sh` 比对）

## 许可

**GPL-3.0**，全文见 [LICENSE](LICENSE)。
