# 更新日志 / Changelog

<!--
格式见 AGENTS.md §八：一条一行、符号标性质、中英双份、外层用代码块包住（否则 `#` 会被当成标题、
`*` 和 `+` 会变成同一种圆点，符号的区分就没了）。只写"改了什么"。
-->

## 1.3.0

## 更新内容

```
* 自动输入法：规则页加上/修改规则时会实时显示当前控件信息，并可直接点「点击捕获」抓取
+ 自动输入法的规则列表改成一栏列表 + 右侧详情（规则多了不用一路往下翻）
+ 全机器只允许一个 Apex（重复启动被忽略，不论从哪个文件夹启动）
+ 新增 `apex/deploy.sh`：一条命令完成构建、部署、启动
# 修复部署会删掉用户的设置文件和规则
# 修复测试工具会移动用户的鼠标
# 修复自动输入法会在 Apex 自己的设置面板里切换输入法
# 修复自动输入法的诊断日志（改用插件自己的文件夹、每次启动重写、带时间戳，不再每次轮询都写）
# 修复自动输入法新建规则时，打开编辑的可能是另一条规则
# 修复自动输入法保存没生效却显示「已保存」
# 修复自动输入法点「保存」后规则要等一秒才写入文件
+ 自动输入法：新建规则后直接进入捕获状态，提示写在「点击捕获」右侧
+ 自动输入法的所有提示统一到「点击捕获」按钮右侧，改成不抢眼的小字
+ 自动输入法：规则计数靠右，规则可以直接拖动排序（上下按钮去掉）
+ 自动输入法：新建的规则放在列表最上面
+ 自动输入法：切换热键改成点一下再按键来录制，可用就立即保存
+ 自动输入法：每个条件字段前的复选框回来了（勾上才生效，新规则只勾「进程名」）
+ 自动输入法：规则的「启用」开关移到列表每一行右侧
+ 自动输入法：规则名只显示进程名，不带扩展名
+ 自动输入法：规则顺序完全由拖动决定，优先级只作自己的记录、不再影响顺序
+ 自动输入法：去掉页面顶部的状态栏，参数行距收紧
# 修复自动输入法录不到 Ctrl+Space（系统占用这个组合键时，焦点被抢走就录不下来）
# 修复「打开设置文件」打不开（现在打开插件自己那份 config.json）
+ 版本号：自动输入法 v1.2.1、滑动滚轮 v1.7.1（沿用各自独立应用的版本号）；主程序 1.0 显示在左侧标语右侧
+ 明暗两套界面的「选中」改成同色系的浅色调，不再黑白反差
# 修复切换功能页时，旧页面的数据会画到当前页上（捕获等待期间的轮询答复缺少归属标记）
# 「打开设置文件」失败时会在宿主日志里写明原因（以前点了没反应，日志里也查不到）
+ 通用设置新增「开机启动」（默认关闭；这是 Apex 唯一写在自身文件夹之外的东西）
+ 插件页标题的版本号后面，加一行小字说明这个插件是做什么的
+ 规则列表与参数页里的小开关改小，和插件列表的开关一样大
+ 通用设置：选项名居左、设置项居右，中间空白随窗口大小伸缩；「开机启动」放在第一项
+ 全程序的滑动开关统一配色：绿为开、灰为关（和插件列表的状态点同一套颜色）
# 修复设置面板刚打开时有一段空白（浏览器还没起来就把窗口显示出来了，现在等页面画好第一帧再显示）
# 修复「正在连接」被显示成红色警告「Apex 主程序没有在运行」（面板刚打开的瞬间还没拿到快照）
# 修复拖动滑块时曲线要等松手才动（改成拖动中每 80ms 重画一次曲线，且只重画曲线、不重建页面）
+ 设置面板改成「右键托盘图标时就先悄悄启动」：点「设置」几乎立刻出现（实测 25ms）；用不到就自己退出，不常驻
+ 明暗两套界面改成近乎中性（饱和度 1.2%–5%）：浅色是淡白、深色是淡黑，不再发黄；图标里的米色不变
+ 明暗两套界面的底色彻底去掉暖调：饱和度 0（纯灰），开关与状态点的语义色保持不变
+ 浅色图标换成一版近白底板的图稿（用户重画），生效时中间竖条显示为大红（两种主题同一个红）
+ 新功能「保持唤醒」：一张程序列表，每一行有「保持唤醒」和「阻止熄屏」两个开关和一个移除按钮
+ 保持唤醒：列表里固定有一行「系统全局」（不可删除），原来的「阻止系统睡眠 / 阻止屏幕关闭」两个总开关就在这一行
+ 保持唤醒：每行只有两种状态——只保持唤醒，或者保持唤醒 + 阻止熄屏（屏幕不关就不可能睡眠，两个开关因此是联动的）
+ 保持唤醒：列表叠加取最强的一行；一行只保持唤醒、另一行两项都有，就按两项都有的那行执行
+ 保持唤醒：插件页顶部实时显示现在在阻止什么、是哪一行在起作用
+ 保持唤醒：托盘图标分三态——什么都不管是原样，保持唤醒是绿色竖条，保持唤醒 + 阻止熄屏是红色竖条
+ 保持唤醒：设置文件换了格式（`global_awake` / `global_display` / `item=名字|保持唤醒|阻止熄屏`），旧文件会被自动翻译过来
+ 设置面板新增一种列表画法（`rows`：一行一项、字段直接可改、每行一个移除按钮），供「保持唤醒」使用
+ 功能接口（ABI）升到 9：新增"更强一档的保持"状态位，以及列表控件的四种新表达（见 `apex/abi.h`）
# 保持唤醒：「添加」这一行移到程序列表卡片外面、列表上方（原来是卡片里标题下面）
# 保持唤醒：添加框左边与列表卡片的左边缘对齐
# 修复保持唤醒「系统全局」那行的两个开关比别的行靠右（那行没有删除按钮，现在把位置留出来了）
+ 保持唤醒：「系统全局」这几个字现在随中/英文界面切换（英文是 System-wide）
- 去掉插件页的实时读数（那个「保持唤醒 · 屏幕常亮（…）」的提示）：整条链路一起删掉，不是只藏起来
+ 所有「添加」按钮不再做成撞色（原先是一个近黑/近白的实心块），改成比「删除」略深一档的底色来区分；滑动滚轮、自动输入法、保持唤醒三处一致
# 修复保持唤醒的程序数量把「系统全局」也数了进去（现在只数你自己添加的程序）
# 自动输入法：新建的规则没点保存就切走，等于没建（那条空规则一起消失，也不会写进 config.json）
# 自动输入法：「编辑/保存」按钮不再做成撞色（新建规则时参数面板里那个近黑/近白的实心块），改成比「添加」再深一档
# 修复插件页的实时读数搬进列表栏后被下一次重绘销毁（页面每秒报一次「missing element」，实测 2122 次）；该读数随后已整体删除
# 修复插件页在每次 listOp 之后的快照里会被清空一下（暗色下就是一块黑，新建规则时最明显）：重读当前页不再丢掉手上的控件文档
# 修复捕获完成后参数页不刷新（要点一下规则条才出现）：等待结束改由功能自己报告（ABI 11 的 waiting），不再靠"文档变了"猜
+ 功能接口（ABI）升到 11：列表控件可以声明"某个操作还在等"，面板照它决定要不要继续轮询
# 修复构建过程会往 stderr 写字，让成功的部署显示成失败（去掉自动输入法那条无法处理的命名警告，cargo 的进度也不再输出）
+ AGENTS.md 拆成「每天必读的核心 + 按主题的 8 个分册」（原来 126 KB，一次会话只读得进约 64 KB，一半以上到不了眼前）
+ 新增门 `check_apex_docs`：核心文件超过 48 KB、索引里的分册不见了、出现没人索引的分册、关键章节丢了，都会红
+ 新增 `test/timing.sh`：量一次「构建 + 门」各花多久（它不是门，不会让任何东西变红）
# 部署时不再反复闪烁：门起的私有拷贝不再往托盘放图标、也不再显示面板窗口（原来一次交付要闪 18 次）
+ 两个只给测试用的开关：`APEX_NO_TRAY`（宿主不放托盘图标）与 `APEX_NO_WINDOW`（面板不显示窗口）
# 修正 AGENTS 里"一份安装只跑一个宿主"的判据：认的是**可执行文件名**（全机器一个），不是文件夹——旧规则的残留
# 部署不再每次都跑全套门：按"这次到底变了什么"选门（只改文档 46 秒、只改面板 80 秒、改了宿主或功能 140 秒；原来固定 258 秒）
# 部署只覆盖真的变了的文件（没变的逐行写 `unchanged, left alone`）
# 装配层的固定 sleep 从 57 秒降到 15.5 秒；「编辑/保存」那扇门从 56 秒降到 10.8 秒（探针一直在等一份根本不会来的文档）
# 交付过程中不再弹窗口（装配层里起面板的三扇门都改成不显示窗口：原来会弹 3 次，现在 0 次）
# 修复：部署（或门把现场还回去）时启动的那份 Apex，会跟着运行它的那个工具一起退出；改成用 WMI 服务启动
+ `test/timing.sh --assembly`：量装配层每一扇门各花多久（原来只量构建层）
# 构建只在真的需要时才做：无改动的构建 3.2 秒 → 2.05 秒（两个 exe 原先每次都重新链接、`apex_owners.exe` 每次都重编译）
+ 设置界面拆成三个源文件：`ui/panel.html`（外壳）+ `ui/panel.css` + `ui/panel.js`，构建时拼成一份嵌进面板；页面字节与拆之前完全一致
* 新功能「媒体控制」：每个显示器一个亮度拉杆，每个显示器可以单独熄屏
+ 媒体控制：亮度按 DDC/CI、WMI、gamma 三级自动选，能真调背光就真调（本机主屏走 WMI，副屏走 gamma）
+ 媒体控制：熄屏是覆盖该显示器的一块纯黑窗口——不断开、不锁屏、不动系统电源；在黑屏上点任意一下、按 Esc、或该屏自己的全局快捷键都能亮回来
+ 媒体控制：每个显示器一个熄屏快捷键，默认空
+ 媒体控制：音量就是系统音量合成器——每个应用一个拉杆和一个静音开关，跟随当前默认声卡
+ 媒体控制：快速面板里「亮度」和「音量」各成一组，每组一个映射开关（默认关闭）；熄屏不进快速面板
+ 媒体控制：设置面板里的亮度和音量是推子，熄屏快捷键点一下就能录
+ 媒体控制：显示器和应用的名单会自动刷新（拔插显示器、启动/退出程序都不必重开面板）
+ 媒体控制：换了物理显示器会重新判断用哪种方式调亮度；判断失败（刚开机、刚插上）会定期重试
+ 媒体控制：解除熄屏后，设置面板上的熄屏开关会自己跟着关掉
+ 快速面板：太长的设备名/程序名，鼠标停上去会来回滚动显示
+ 功能接口（ABI）升到 13：放进快速面板的控件可以声明自己属于哪一组（组名中英双语）
+ 媒体控制：显示器名的省略号和换行修好了；每行可以给显示器和应用起自己的名字，名字会显示到快速面板
+ 媒体控制：亮度拉杆是一个绝对亮度（基准归一到最亮），与别的调光工具不再相乘——两边都拉到最暗不会再"看不见"
+ 媒体控制：用系统快捷键/显示器按钮调亮度时，推子会跟着实时更新（功能每两秒读回屏幕真实亮度）
+ 媒体控制：设置快捷键时，Del 或 Backspace 可以清掉
+ 媒体控制：快速面板和设置页里的「系统声音」现在跟随语言（英文界面显示 System sounds）
# 修复设置页里给显示器/应用加名字以后整行被挤变形（设备名换行、"系统声音"竖排）
# 修复 gamma 被别的调光工具压过时，本功能的百分比是在别人基础上再乘一层
# 修复亮度写入被系统接受但屏幕没有真的变化时，功能认为一切正常（现在会读回验证并改走别的协议）
+ 媒体控制：用系统快捷键调亮度时，推子跟得更快（跟随期间每 0.4 秒读一次），快速面板里的推子也会跟着动
+ 媒体控制：自定义名字的输入框移到每行「亮度」「音量」的位置，去掉「名字」这个提示
# 修复外接屏（HKC）的亮度调不动：DDC 句柄不再长期持有，每次调用现取现还
# 修复快速面板打开后，里面的值不会跟着外部变化更新
# 修复快速面板定期刷新时会打断鼠标悬停和名字滚动
+ 媒体控制：显示器的设置改按「显示器自身的身份」（厂商+型号+序列号）保存，拔插或换口不会再串到别的屏
+ 媒体控制：开机之后才插上的显示器，现在也能取回它自己的亮度和名字
+ 媒体控制：跟随系统亮度键更快了（功能与面板的读取间隔都从 0.4 秒缩到 0.2 秒）
# 修复副屏的自定义名字存不住（每次重新探测都用设置文件里的旧值把它盖掉）
# 修复「系统声音」那一行在有的机器上显示成「(pid 8)」（改用系统的官方判断，不看 pid）
# 修复录完熄屏快捷键之后整行错位、控件顶出面板（快捷键框没有自己的宽度）
# 修复给设备起名字之后一行放不下（名字框宽度减半，去掉与设备名重复的「亮度」「音量」标签）
+ 功能接口（ABI）升到 14：列表可以声明「这些行不能新建」（显示器、应用、熄屏快捷键那三组）
# 修复媒体控制的插件页是空白的（交出去的控件文档多了一个逗号，页面解析不了）
# 修复副屏亮度拉杆的下半段动了却没反应（Windows 不允许 gamma 白点低于满量程一半，现在拉杆的行程映射到可用区间）
# 修复媒体控制与另一个调光工具互相覆盖（每次改亮度前重读 gamma，屏幕此刻的样子成为新的基准；退出时也不再覆盖别人）
# 修复控件文档的检查只盖住了一个功能：现在每个已构建的功能的文档都要真的被解析一遍
# 修复控件文档的检查把「括号配平」当成「是合法 JSON」：现在还要过一遍真正的解析器
# 修复设置面板里组内的字段只按文本框画（推子和快捷键录制框都被画成了文本框）
# 修复显示器/应用/熄屏快捷键三组上面多余的「添加」按钮（这些行本来就不该能新建）
# 修复换显示器后仍沿用上一块屏的亮度判断（外接屏明明支持 DDC，却一直走 gamma 而且拉杆无效）
# 修复 gamma 写入被系统拒绝后不再尝试别的亮度方式（现在会重新探测硬件）
+ 媒体控制：熄屏和解除熄屏各有 0.5 秒的渐变（黑窗口的透明度，不是一下跳过去）
+ 快速面板：设备名一栏固定为五个汉字宽，省出来的宽度给推子（推子从 120 px 变成 200 px）
+ 快速面板：点推子旁边不再改值——只有点在推子自己身上才算
+ 快速面板：推子支持鼠标滚轮改值（每个推子都是，一格一步，和设置页一致）
+ 快速面板：音量推子右边加了静音按钮
+ 快速面板：亮度推子右边加了该显示器的熄屏按钮（按下就是黑屏 / 亮回来）
+ 设置面板：三个列表标题右边的计数去掉了（那些行是机器给的，"有几个"不是问题）
+ 设置面板：设备名一栏按内容缩到最短，省出的宽度给推子，同一块面板里的控件仍然对齐
+ 设置面板：熄屏快捷键那一行去掉「快捷键」文字；输入框改成刚好装下快捷键的宽度并靠右对齐
+ 功能接口（ABI）升到 15：推子可以带一个同伴开关（音量行的静音按钮）
+ 功能接口（ABI）升到 16：那个同伴开关可以声明自己是什么（静音 / 熄屏），面板照它选图标
+ 设置面板：窗口宽度有了下限（800×520，随显示器缩放），往小拉不会再出现控件超出面板
+ 保持唤醒：「添加」按钮右侧多了一个「快速面板」开关；打开后快速面板按列表显示系统全局和每个程序的两个开关
+ 媒体控制：「快速面板」开关移到「亮度」「音量」各自小标题的右侧
+ 滑动滚轮：「快速面板」开关合成一个（放在「最高速度」下面），映射到快速面板的是四个参数的推子
# 修复媒体控制的「刷新应用列表」按钮从来没有显示出来（这种列表的按钮栏被整条丢掉）
+ 功能接口（ABI）升到 17：整个组可以共用一把「快速面板」开关；同伴开关也可以画在开关行上
+ 媒体控制：应用音量列表自己实时刷新，「刷新应用列表」按钮去掉
+ 快速面板：不透明度从 78% 提到 90%（没有模糊可用，太透看起来是贴纸而不是玻璃）
+ 功能接口（ABI）升到 18：一个组可以声明「这些行是页面外面那个东西的实时写照」，面板自己重读
+ 快速面板：各分组之间的间距从 16 收到 3（缝里放不下阴影，分组阴影因此去掉）
+ 通用设置：快速面板下面新增一个小面板，列出当前有映射的功能，可拖动调整它们在快速面板里的顺序（两个总开关固定在上面，不参与排序）
# 修复宿主发给面板的快照漏了一个 `]`（面板会把「宿主没在运行」显示出来）；`check_apex_persist` 现在真的解析一次那份快照
+ 通用设置：那个小面板按**单个开关**（一组）列，不是按插件；进入通用页时会重新向宿主读一次，所以跟着映射开关实时变
+ 快速面板：插件总开关关掉后，它的局部控件整个隐藏（再打开时按各局部开关的状态原样回来，位置也留在原处）
+ 保持唤醒：快速面板里「阻止熄屏」改用和「保持唤醒」一样的滑动开关（原来是带图标的小按钮）
# 修复深色主题下熄屏按钮上的显示器图标看不见（白图标画在近白底上，按钮成了一块空白方块）
+ 快速面板：开关做小，和设置面板的开关一样大（26×15）
# 修复通用设置里快速面板分组的顺序改不动（整串新顺序被宿主当成多行，只剩下第一个键）
+ 版本号：主程序 1.3（设置界面标语旁显示的那个），媒体控制 1.3.0
```

## What's changed

```
* AutoIME: the rules page shows the live control read-out, with a Capture button that fills a rule from the next click
+ AutoIME: the rule list is now a list with a detail pane (no more scrolling past every rule)
+ One Apex per machine; a repeat launch from any folder is ignored
+ New `apex/deploy.sh`: build, deploy and start in one command
# Fixed a deploy that deleted the user's settings files and rules
# Fixed the test tools moving the user's mouse
# Fixed AutoIME switching the input method inside Apex's own settings panel
# Fixed AutoIME's diagnostic log (its own folder, rewritten every run, timestamped, no longer one write per poll)
# Fixed AutoIME opening a different rule when a new one was added
# Fixed AutoIME reporting "Saved" when the save had not taken effect
# Fixed AutoIME taking a second to write a rule after Save
+ AutoIME: a new rule goes straight into capture mode, with its hint beside the Capture button
+ AutoIME: every notice now appears beside the Capture button, as a quiet hint
+ AutoIME: the rule count sits at the right; rules are reordered by dragging (the up/down buttons are gone)
+ AutoIME: a new rule is added at the top of the list
+ AutoIME: the toggle hotkey is recorded by pressing it, and saved as soon as it is usable
+ AutoIME: every condition has its tick box again (a value counts only when ticked; a new rule ticks Process)
+ AutoIME: a rule's enable switch moved to the right-hand end of its row in the list
+ AutoIME: a rule is listed by process name, without the extension
+ AutoIME: the order is dragged and nothing else; the priority is your own note and no longer orders anything
+ AutoIME: the status box at the top of the page is gone and the rows are tighter
# Fixed AutoIME not recording Ctrl+Space (the system owns that combination, so the focus was taken away)
# Fixed "open settings file" opening nothing (it opens the feature's own config.json now)
+ Versions: Auto IME v1.2.1, Smooth Wheel Scroll v1.7.1 (the versions of the standalone programs they came from);
  the host's own 1.0 is shown beside the slogan in the sidebar
+ The selected state in both appearances is a tint of that appearance, instead of black-on-light / white-on-dark
# Fixed the old page's data being drawn on the new one when switching features (a polled answer had no owner)
# "Open settings file" now says why it failed, in the host's log (before: nothing happened, and nothing was logged)
+ General settings has "Start with Windows" (off by default; the only thing Apex writes outside its own folder)
+ A feature's page shows one small line under its version, saying what the feature is for
+ The small switches in the rule list and the parameter page are now the size of the feature list's switch
+ General settings: the name on the left, the control on the right, the gap between them following the window;
  "Start with Windows" is the first item
+ Every switch in the program uses the feature list's colours: green for on, grey for off
# Fixed the blank moment when the settings panel opens (the window was shown before the browser existed; it is
  now shown once the page has drawn its first frame)
# Fixed "connecting" being shown as the red "the Apex host is not running" warning (no snapshot had arrived yet)
# Fixed the curve waiting for the mouse button to be released (it is now redrawn every 80 ms during a drag, and
  only the curve is redrawn -- the page is not rebuilt)
+ The settings panel is started quietly when the tray menu opens, so choosing Settings appears almost at once
  (measured 25 ms); if it is not used it exits by itself and nothing stays resident
+ Both appearances are nearly neutral now (saturation 1.2%–5%): a pale white and a pale black, with no yellow
  cast left; the cream inside the icon is unchanged
+ New feature: Keep Awake -- one list of programs, each row carrying "keep awake" and "keep the screen on"
  switches plus a remove button
+ Keep Awake: the list always holds a "system-wide" row (not removable), which is where the old two master
  switches now live
+ Keep Awake: a row is either awake-only or awake + screen-on (a screen that stays on cannot be a machine that
  sleeps, so the two switches move together)
+ Keep Awake: the rows combine by taking the strongest one -- one row awake-only and another with both means
  both
+ Keep Awake: the top of its page says what is being blocked right now and which row is doing it
+ Keep Awake: the tray mark has three states -- plain when nothing is held, a green bar while keeping awake,
  a red bar when the screen is being kept on as well
+ Keep Awake: its settings file changed shape (`global_awake` / `global_display` / `item=name|awake|display`);
  a file from the previous version is translated on the way in
+ The settings panel gained a list layout (`rows`: one line per item, fields edited in place, a remove button
  per line), used by Keep Awake
+ The feature ABI went to 9: a flag for the stronger kind of hold, and four new things a list control can say
  (see `apex/abi.h`)
# Keep Awake: the Add row moved outside the program list, above it (it was inside the card, under the heading)
# Keep Awake: the add box's left edge lines up with the list card's left edge
# Fixed the "system-wide" row's switches sitting further right than every other row's (that row has no remove
  button, and its place is now held open)
+ Keep Awake: the "system-wide" row now follows the interface language (System-wide in English)
- Removed the feature read-out (the "keeping awake, screen stays on (...)" line): the whole path went, rather
  than the box being hidden
+ No "Add" button wears the accent chip any more (it used to be a near-black / near-white block); it is one step
  of surface darker than Delete instead -- the same in SmoothWheel, AutoIME and Keep Awake
# Fixed Keep Awake's program count including the "system-wide" row (it counts the programs you added)
# AutoIME: a new rule that is not saved is not created -- switching to another rule drops the empty rule too, and
  it is never written to config.json
# AutoIME: the Edit/Save button no longer wears the accent chip (the near-black / near-white block that appeared
  in the parameter pane when a new rule opened for editing); it is one step stronger than Add instead
# Fixed the feature read-out being destroyed by the next redraw after it was moved into the list bar (the page
  reported "missing element" once a second -- measured 2122 times); the read-out itself has since been removed
# Fixed the feature page being blanked for one render by the snapshot that follows every listOp (a #141414
  rectangle in the dark theme; it showed up as soon as a rule was created); re-reading the page you are already
  on no longer throws the document away
# Fixed a finished capture not reaching the parameter pane until the rule row was clicked: the feature now says
  that it is still waiting (ABI 11, `waiting`) instead of the page guessing from "the document changed"
+ The feature contract (ABI) is now 11: a group may declare that an action is still waiting, and the panel
  polls on that
# Fixed the build writing to stderr, which made a successful deploy report a failure (the unactionable AutoIME
  naming warning is gone, and cargo no longer prints its progress)
+ AGENTS.md is now a short core plus 8 topic books (it had grown to 126 KB, while a session can read in about
  64 KB -- so more than half of the rules never reached the reader)
+ New gate `check_apex_docs`: the core growing past 48 KB, a topic book the index names going missing, an
  unindexed topic book, or a lost section -- each one turns it red
+ New `test/timing.sh`: measures where "build + gates" actually goes (it is not a gate and never fails)
# No more flickering during a delivery: the private copies the gates start no longer put an icon in the tray or
  show a panel window (a delivery used to flash 18 of them)
+ Two test-only switches: `APEX_NO_TRAY` (the host adds no tray icon) and `APEX_NO_WINDOW` (the panel is never
  shown)
# Corrected the "one host per installation" rule in AGENTS: the identity is the EXECUTABLE NAME (one per
  machine), not the folder -- what was written there was left over from the older rule
# A delivery no longer runs every gate: the set is chosen from what actually changed (docs only 46 s, panel only
  80 s, host or feature 140 s -- it used to be 258 s whatever changed)
# A delivery copies only the files that really differ (the rest are logged as `unchanged, left alone`)
# The assembly layer's fixed sleeps went from 57 s to 15.5 s; the edit/save gate from 56 s to 10.8 s (its probe
  was waiting for a document that those commands never send)
+ `test/timing.sh --assembly`: times every gate, assembly layer included (it used to time only the build layer)
# The build only does what is needed: a no-change build went from 3.2 s to 2.05 s (both exes were relinked every
  time and `apex_owners.exe` was recompiled every time)
+ The settings page is three source files now -- `ui/panel.html` (the shell) + `ui/panel.css` + `ui/panel.js`,
  assembled into one file at build time; the embedded page is byte-for-byte what it was
# A delivery no longer pops windows up (the three assembly gates that start a panel now keep it hidden: it used
  to be 3 pop-ups, it is 0 now)
# Fixed the deployed copy exiting along with whatever tool ran the deploy (or handed the field back); it is
  started through the WMI service now
* New feature "Media Control": one brightness fader per monitor, and each monitor can be turned off on its own
+ Media Control: brightness picks its protocol by itself -- DDC/CI, then WMI, then the gamma ramp (the main
  screen answers WMI, the second one only gamma)
+ Media Control: "screen off" is an opaque black window over that one monitor -- nothing is disconnected, the
  desktop does not lock, no power state changes; a double click on it, a click and Escape, or that monitor's own
  global shortcut brings it back
+ Media Control: one screen-off shortcut per monitor, empty by default
+ Media Control: the volume mixer itself -- a fader and a mute switch per application, following whichever
  output device is the default
+ Media Control: the quick panel gains one pane called Brightness and one called Volume, each behind its own
  mapping switch (off by default); screen-off is deliberately not in there
+ The feature ABI is now 13: a control sent to the quick panel can say which pane it belongs to (named in both
  languages)
# Fixed Media Control's feature page being blank (the controls document it sent had a doubled comma in it)
# Fixed the bottom half of the second screen's brightness fader moving without changing anything (Windows refuses
  a gamma white point below half of full scale; the slider's travel is mapped into what the API accepts)
# Fixed Media Control and another dimming tool overwriting each other (the ramp is read back before every change,
  and whatever is on the screen becomes the new baseline; letting go no longer throws the other tool's setting away)
# Fixed the controls-document check covering only one feature: every built feature's document is parsed now
# Fixed that check treating "the brackets balance" as "it is JSON": a real parser runs over it as well
# Fixed fields inside a group being drawn as text boxes whatever they were (a fader and a shortcut recorder both
  came out as plain inputs)
# Fixed the extra "Add" button above the monitor, application and shortcut groups (those rows cannot be created)
# Fixed a swapped monitor keeping the previous screen's brightness verdict (an external panel that answers DDC/CI
  was left on the gamma fallback, where its fader did nothing)
# Fixed a refused gamma write ending the search for a working brightness path (it now asks for a fresh probe)
+ Media Control: the screen comes back on a single click anywhere on the dark screen (it used to take two double
  clicks), Escape does the same, and both act at once rather than on the next tick
+ Media Control: the brightness and volume controls in the settings page are faders, and a screen-off shortcut is
  recorded by pressing it
+ Media Control: the monitor and application lists keep themselves up to date (plugging a screen in, or starting
  and quitting a program, no longer needs the page reopened)
+ Media Control: a screen that has been replaced is probed again, and a failed verdict is retried
+ Media Control: turning a screen back on makes the settings page's own switch follow
+ The quick panel scrolls a device or program name that is too long for its row while the pointer rests on it
+ The feature ABI is now 14: a list can declare that its rows are not the user's to create
+ Media Control: turning a screen off and bringing it back are half-second fades now, not a jump
+ Quick panel: the device-name column is five characters wide, and the width it frees goes to the fader
+ Quick panel: a click beside a fader no longer moves it -- only a click on the fader itself does
+ Quick panel: every fader takes the mouse wheel (one step per notch, the same as the settings page)
+ Quick panel: a mute button beside each volume fader
+ Quick panel: a screen-off button beside each brightness fader (that monitor goes dark, or comes back)
+ Settings panel: the row count is gone from the three lists whose rows the machine supplies
+ Settings panel: the device-name column shrinks to what its names need, and the width goes to the fader --
+  controls still line up inside each card
+ Settings panel: the screen-off shortcut row lost its "Shortcut" label, and the box is now just as wide as the
+  combination it holds, right-aligned
+ The feature ABI is now 15: a fader can carry a companion switch (the mute button on a volume row)
+ The feature ABI is now 16: that companion says what it is (mute / screen off), and the panel picks its icon
+ Settings panel: the window has a minimum size now (800x520, scaled by the monitor), so making it narrow can no
+  longer push a plugin's controls out of the panel
+ Keep Awake: one "Quick panel" switch beside the Add button; with it on, the flyout lists system-wide and every
+  program with both of its switches
+ Media Control: the "Quick panel" switch moved to the right of the Brightness and Volume headings
+ Smooth Wheel Scroll: the quick-panel switches are one switch now (below Top speed), mapping the four
+  parameters as faders
# Fixed Media Control's "Refresh applications" button never appearing (that list's button bar was thrown away)
+ The feature ABI is now 17: a whole group can share one quick-panel switch, and a companion switch can be drawn
+  on a switch row as well
+ Media Control: the application volume list keeps itself up to date, and the "Refresh applications" button is
+  gone
+ Quick panel: opacity raised from 78% to 90% (there is no usable blur, and too much see-through reads as a
+  sticker rather than glass)
+ The feature ABI is now 18: a group can declare that its rows are a live picture of something outside the page,
+  and the panel re-reads it by itself
+ Quick panel: the gap between the blocks went from 16 to 3 (a shadow does not fit in that seam, so the blocks
+  have none now)
+ General settings: a small panel under the quick-panel switches lists the features that are mapped into the
+  flyout, and they can be dragged into the order they appear in (the two master switches stay on top and are not
+  part of that order)
# Fixed a missing `]` in the snapshot the host sends the panel (the page showed "the Apex host is not running");
+  check_apex_persist now really parses that snapshot
+ General settings: that panel lists PANES (one per switch/group), not plugins, and the list is re-read from the
+  host whenever the page is opened, so it follows the mapping switches
+ Quick panel: switching a feature off hides its local controls entirely (switching it back on restores them from
+  their own mapping switches, in their original place)
+ Keep Awake: the flyout's "keep the screen on" control is the same sliding switch as "keep awake" (it used to be
+  a small button with a monitor icon)
# Fixed the monitor icon on that button being invisible in the dark theme (a white glyph on a near-white fill, so
+  the button came out as a blank square)
+ Quick panel: the switches are smaller, the same size as the settings page's (26x15)
# Fixed the quick-panel groups not reordering on the General page (the whole new order was read as several lines,
+  so only the first key arrived)
+ Version numbers: the program reports 1.3 on the settings page, and Media Control 1.3.0
```

## 1.2.0

## 更新内容

```
* 新增功能「自动输入法」：按当前焦点控件自动切换中英文输入法
+ 功能插件不再限于 C++，可以用 Rust 写
+ 功能可以询问自己是否被停用（自带线程的功能需要）
```

## What's changed

```
* New feature "Auto IME": switches the input method by the focused control
+ A feature plugin no longer has to be C++ -- Rust is supported too
+ A feature can ask whether it is switched off (needed by one with its own threads)
```

## 1.1.0

## 更新内容

```
+ 黑名单改名「排除」
+ 检测到 REAPER 插件在运行时，在「排除」旁给出提示
+ 插件列表显示状态点：绿色启用、灰色停用、红色禁用
+ 同一文件夹只运行一个实例，重复启动被忽略
```

## What's changed

```
+ Renamed the blacklist to "Exclude"
+ A note beside "Exclude" when the REAPER plugin is running
+ Feature list shows a state dot: green enabled, grey off, red disabled
+ One instance per folder; a repeat launch is ignored
```

## 1.0.0

## 更新内容

```
* 全局接管鼠标滚轮：任何程序里滚动都平滑，带加速与收尾
* 设置面板：中英双语，明暗跟随系统
* 运动曲线图：四个滑块各管一段颜色，参数一动手就跟着变
* 功能插件化：一个功能一个文件夹，面板照着功能自己的描述画控件
+ 黑名单：列进去的程序原样放行，支持 * 与 ? 模糊匹配
+ 插件列表显示状态点：绿色启用、灰色停用、红色禁用
+ 滑块支持滚轮改值，双击恢复默认
+ 托盘图标随语言与系统明暗变化
+ 设置改完即时落盘，不必等退出
+ 同一文件夹只运行一个实例，重复启动被忽略
# 修复快速滚动时的顿挫
# 修复关闭托盘程序后残留的面板与浏览器进程
# 修复面板打开时先闪一下白
# 修复浅色主题偏黄
# 修复面板上的滚轮被平滑，导致滑块无法用滚轮改值
```

## What's changed

```
* Global mouse-wheel takeover: smooth scrolling in any program, with acceleration and settle
* Settings panel: bilingual, follows the system light/dark theme
* Motion chart: each of the four sliders owns a coloured segment, and the curve follows every change
* Features are plugins: one folder each, and the panel draws whatever a feature describes
+ Blacklist: listed programs are passed through untouched, with * and ? pattern matching
+ Feature list shows a state dot: green enabled, grey off, red disabled
+ Sliders take the mouse wheel, and a double-click restores the default
+ The tray icon follows the language and the system theme
+ Settings are written to disk as soon as they change, not on exit
+ One instance per folder; a repeat launch is ignored
# Fixed the lurch during fast scrolling
# Fixed panel and browser processes surviving the tray program
# Fixed a white flash when the panel opens
# Fixed the light theme reading as too yellow
# Fixed the wheel over the panel being smoothed, which stopped sliders taking the wheel
```
