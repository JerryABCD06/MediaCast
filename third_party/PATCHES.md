# third_party 的本地改动

这个目录里的东西都是**第三方源码**，原则上不动 —— 改了，上游一升级就会冲突。

但个别情况下不得不动。凡是动过的，都记在这里：**改的是哪个文件、改了什么、
为什么、升级后怎么重新打**。

> **2026-09-14 起，FluentUI 整份进了版本控制**（之前是独立 clone、靠这份文档
> 记补丁）。好处是"我们改了它哪几行"在 git 里就是看得见的 diff。它自己那份上游
> `.git/` 改名成了 `.git-upstream/`（留在盘上、不进仓库），源目录里那份
> `.gitmodules` 之类都还在。
>
> 上游：`https://github.com/zhuzichu520/FluentUI.git`
> 基线提交：`7e33a2f672d18239ef49ab960075b1137a1d18e7`（2026-04-07）

升级流程（现在）：

1. 在 `FluentUI/.git-upstream/` 里 `git fetch`，看清上游新提交动了哪些文件
2. 把上游新版本覆盖过来（保留下面这几条补丁，逐条重新打）
3. 整个构建目录删掉重建（不能增量 —— 见仓库根目录那份说明里"改过 .h
   必须全量重建"那条）
4. 跑一遍验证（回归脚本 + 新界面起来看一眼）

---

## 补丁 1：从 FluentUI 的构建里剔除 qmlcustomplot

**文件**：`FluentUI/src/CMakeLists.txt`

**位置**：紧跟在已有的
`list(REMOVE_ITEM sources_files qhotkey/qhotkey_mac.cpp ...)` 那一行后面
（也就是遍历完所有 .cpp/.h 之后、Qt6 那一大段之前）。

**加了什么**：

```cmake
list(FILTER sources_files EXCLUDE REGEX "^qmlcustomplot/")
```

**为什么**：

`src/qmlcustomplot/` 里打包了一整个 qcustomplot 绘图库。光
`qcustomplot.cpp` 一个文件就有 1.3 MB 源码，编译出 **14.2 MB** 的目标文件 ——
是整个构建里最慢的一步，实测会把 Qt Creator 的进度条卡在 60% 附近好几分钟，
同时把内存吃满（16 核并行时尤其明显，整个界面都会发木）。

它往 QML 里注册 6 个类型：`BasePlot` / `TimePlot` / `RealTimePlot` /
`PlotGrid` / `PlotAxis` / `PlotTicker`。全库搜过，**只有 FluentUI 自己的示例
用得到**（`example/qml/page/T_CustomPlot.qml` 和 `T_CustomPlot2.qml`），
而我们是 `FLUENTUI_BUILD_EXAMPLES=OFF` 编的。

**为什么这样写而不是列文件名**：那个目录里有 17 个文件，一条条列出来，
上游每加一个就漏一个。正则匹配的是相对 `src/` 的路径 —— 上面的
`string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/" "" filename ${filepath})`
已经把前缀去掉了。

**会不会有牵连**：

不会。唯一在 `qmlcustomplot/` 之外引用这些头文件的是 `FluentUI.cpp`，而它在
Qt6 构建里**本来就已经被上游自己剔除了**（同一个 CMakeLists 里有
`list(REMOVE_ITEM sources_files FluentUI.h FluentUI.cpp)`，那是 Qt5 才用的）。

`target_include_directories` 里那行 `qmlcustomplot` 留着没动 —— 一个用不到的
包含路径，没有副作用，改了反而多一处差异。

**代价**：以后要是想用它的图表组件（不是 `FluChart` ——那个是 Canvas +
Chart.js，跟这个无关），得把这一行去掉再重建。

---

## 补丁 4：标题栏那三个按钮改成 Windows 标准的样子

**文件**：`FluentUI/src/Qt6/imports/FluentUI/Controls/FluAppBar.qml`

**改了什么**：

| 项 | 原来 | 改成 |
|---|---|---|
| 条高（库默认） | 30 | **32**（微软《标题栏设计》：标准标题栏 32px） |
| 最小化 / 最大化 / 关闭的宽度 | 40 | **46** |
| 上述三个的高度 | 写死 30 | **`Layout.fillHeight`**（跟着标题栏高度走） |
| 最小化 / 最大化图标 | 11 | **10** |
| 关闭键悬停色 | `rgba(251,115,115,1)` 浅珊瑚 | **#C42B1C** |
| 关闭键按下色 | 同上 0.8 透明 | **#C53D30** |
| 关闭键字形 | 只在 hover 时转白 | hover **或按下**都转白 |
| 库里那两个额外键（深色切换 / 置顶） | 高度写死 30 | 也跟着 `fillHeight`（宽度仍是 40） |

**为什么**：他照着微软那篇文章核对过，现在这几个按钮"不标准"。文章给了条高（32）、
四个字形码位和"标题按钮必须有完整出血背板"这条规矩，但**没写按钮宽度** —— 宽度
46、图标 10 是拿真机量的：他给的一张 100% 缩放截图里，悬停背板 x 96..141（宽 46）、
y 0..31（高 32），三个字形都是 10 高、中心间距 46.0 / 46.0。

**两处有意为之、别改回去**：

1. **高度不写死**。文章要求背板铺满整条标题栏，而条高各窗口可以不一样
   （我们的主界面是 48，照 Windows「照片」定的）—— 写死就把这条规矩废了。
2. **悬停/按下用主题那套** `itemHoverColor` / `itemPressColor`，不写死灰色。
   Windows 那边量出来约 4.8% 的黑，主题这套是 6%（浅色黑、深色白），差 1.2%
   看不出来；但它是**半透明**的，压在云母、深色主题上都自然。
   只有关闭键那两档红是固定值 —— 深色主题下 Windows 也用同一个红。

**图标**：`ChromeMinimize=0xe921` / `ChromeMaximize=0xe922` / `ChromeRestore=0xe923` /
`ChromeClose=0xe8bb` —— 库里本来就有、码位和文章一字不差，没动。
