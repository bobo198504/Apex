# Apex 1.0.0 — 第一个记录下来的状态

**状态：已交付。** 宿主 + 设置面板 + 唯一的功能（SmoothWheel）。

> **为什么这个文件夹存在**：Apex **还没有仓库**（拆出来时用户没要求建仓），所以**源码没有版本控制兜底**。
> 这个快照就是它唯一的历史。想回到这里，靠的是下面的指纹表。

## 这一版做了什么

| | |
|---|---|
| **宿主** | `apex.exe` —— 全局滚轮钩子、注入线程、帧时钟、托盘、隐藏的 `ApexHostWnd` |
| **面板** | `apex-settings.exe` —— 独立进程，WebView2；关掉宿主就跟着关 |
| **功能** | `Plugins\SmoothWheel\SmoothWheel.dll` —— 平滑滚轮 |

功能清单见仓根 **`CHANGELOG.md`** 的 1.0.0 一节。

## 指纹表

### 产物（`bin/`）—— **逐字节验证过**

| 文件 | 字节 | md5 |
|---|---|---|
| `apex.exe` | 1038533 | `ad6d368264bbeaea7e22a2d25885172b` |
| `apex-settings.exe` | 1236759 | `44d7241186f8f8db1d5c44959ae296a0` |
| `Plugins/SmoothWheel/SmoothWheel.dll` | 102877 | `564a8036f89c752a78a177d6b7a7613b` |

### 源码（`src/`、`build.sh`）

| 文件 | 字节 | md5 |
|---|---|---|
| `src/abi.h` | 13885 | `c8654c3cc6761b4d4fac4d70bc264f40` |
| `src/apex.manifest` | 1019 | `68723f20fd01b1c6d6a90eae2d8132ff` |
| `src/apex.rc` | 2333 | `f3d85a15655aa4d7f68008580300ea90` |
| `src/common/config.h` | 21667 | `b4f7a801b8586ebc1a8899ae129d1076` |
| `src/common/core.h` | 12846 | `e095425fc1cadade283518544c74d2b9` |
| `src/common/release.h` | 4991 | `490f5512bab05a854e976722de9be70d` |
| `src/decision.h` | 9168 | `0c6cf82cb88f4de21ce7f498eb9320ab` |
| `src/features/SmoothWheel/feature_smoothwheel.cpp` | 33947 | `8e941489d82b2a6c921c25315da0f2cf` |
| `src/host.h` | 5004 | `d5774428406d6f82197f3bfca91fa41e` |
| `src/host_win.cpp` | 16950 | `2d57455dd230394368619f74e8ae3f07` |
| `src/hostconfig.h` | 13593 | `d9f493f2691b9dd70fea97ef02b4763d` |
| `src/icons.h` | 4458 | `15218c5d806c2b15ad79cd2f02fe80ac` |
| `src/loader.h` | 3137 | `24c0c33cbbc0d15b3139f6f29847da78` |
| `src/loader_win.cpp` | 5993 | `6bf491efd1c9391968d324ef91dbb7ea` |
| `src/main.cpp` | 55882 | `334b94e927a7d15a368cc595ac6901a8` |
| `src/panel.rc` | 1507 | `e72dc550321ae120935017e1ee6b16db` |
| `src/paths.h` | 5398 | `01c39786d4166a228ae2716dcec3c9ca` |
| `src/paths_win.cpp` | 1141 | `e36a5f120c0a2559db22a26304be7c7c` |
| `src/settings.manifest` | 940 | `cb15859d91edb3f8b550beac386ea2cf` |
| `src/settings_host.cpp` | 18790 | `38c4ec883f357334295922a210c20075` |
| `src/settings_ipc.h` | 14928 | `2901610b71e06f7d0ab9c41383fc5287` |
| `src/settings_main.cpp` | 2127 | `4709a0291cb66ccc1bae2ba739b474f0` |
| `src/system_win.cpp` | 4568 | `9813ecd6613539eabf16501bbd74f73e` |
| `src/ui/panel.html` | 66499 | `e55c361f6dd20e3386e12470462c9c4e` |
| `src/ui_webview.cpp` | 47303 | `1792efd22c493f4f0fd95bef692af6da` |
| `build.sh` | 9752 | `f24a302b277f96bc12c46eea3c1eec7b` |

## ⚠️ 这份源代码是怎么来的（**如实记录**）

**产物是原件；源码是重建的，验证方式是"能否复现产物"。**

1. 快照第一次写好后，我在刷新它时**误删了整个目录**（`rm -rf versions/1.0.0`，没有备份）。
2. **产物从用户正在运行的部署目录取回**（`D:\App protable\Apex`），三个文件与记下的指纹**逐字节相符**——
   原件没丢。
3. 源码是**撤回随后的改动**得到的。**验证不是比对文本，而是比对产物**：
   `rm -rf build/apex/obj && bash apex/build.sh` 之后，三个产物与上面那张表**逐字节相同**。
   能复现，说明这一版源码**在代码层面就是 1.0.0**。
4. ⚠️ **但它不是逐字节的原件**：`ui_webview.cpp` 与 `feature_smoothwheel.cpp` 的**注释**在撤回过程中
   被改写过（代码等价、二进制相同，注释不同）。`panel.html` 是**逐字节原件**——它是嵌在
   `apex-settings.exe` 里的 `RCDATA` 资源，我用一个抽取器把它原样取了出来（md5 与原先记的一致，
   两份独立记录互相印证）。
   **要回到"运行行为上的一模一样"，这份源码是够的；要回到"字节上的一模一样"，只有 `bin/` 和
   `panel.html` 是。**
5. 因此下面这条判据值得记住：**"能复现产物"是比"文本相同"更强的证据**，也是这次能证明源码没走样的原因。
   （它也是唯一可行的，因为注释一旦丢失就找不回来了。）

## 这一版的三个层

改之前先看这张表，能省掉"改了没反应"的一轮：

| 层 | 文件 | 管什么 |
|---|---|---|
| **模型** | `shared/`（插件 src/ 的副本，不在本快照里） | 一格走多远（速度预算）、把量摊开多久（窗口） |
| **小模型** | `src/common/release.h` | **窗口该多长**：单个 = Glide；滚动中 = Glide + 固定 200ms |
| **投递层** | `src/host_win.cpp` | 把 core 交出来的量送进接收方（专用线程 `SendInput`） |
| **判定** | `src/decision.h` | 吞不吞。纯函数，穷举 144 组 |
| **界面** | `src/ui/panel.html` | 面板本身；功能描述自己的控件，页面照着画 |

**`shared/` 不在这里，是有意的**：那三个头是**插件项目**的文件，指纹在仓根 `shared/SOURCE.md`；
抄一份进来只会多一个会漂移的副本。

## 构建与验收

```bash
bash build.sh        # 与仓根 apex/build.sh 同一份脚本（快照里也放了一份）
bash test/run_all.sh # 17 扇门；全过才算交付
```

**交付时 17 扇门全过**，且 `build/apex/` 与 `bin/` **逐字节相同**。

## 已知的边界（不是缺陷，是记录）

- **一份安装一个宿主**：判据是"我这个文件夹"，所以**两份拷贝能并存**。
- **功能列表的红色状态到达不了**：宿主没有"禁用某功能"的能力，这个状态**只留了接口**。
- **黑名单是模式**（`*` / `?`），两头锚定；没有通配符时就是全名精确匹配。
- **面板的滚轮不被平滑**：那是刻意的，否则滑块没法用滚轮改值。
