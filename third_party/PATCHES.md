# third_party 的本地改动

这个目录里的东西都是**第三方源码**，原则上不动 —— 改了，上游一升级就会冲突。

但个别情况下不得不动。凡是动过的，都记在这里：**改的是哪个文件、改了什么、
为什么、升级后怎么重新打**。

升级流程：

1. 删掉 `FluentUI/`，重新 clone 上游
2. 照着下面逐条重新打补丁
3. 整个构建目录删掉重建（不能增量 —— 见仓库根目录那份说明里"改过 .h
   必须全量重建"那条）
4. 跑一遍验证

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
