// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QString>

/**
 * UpnpXml —— DLNA/UPnP 需要的几份 XML 文档。
 *
 * 这些是设备的"自我介绍"和"能力清单"，全是静态文本，只有设备描述需要填两个值
 * （友好名和设备标识）。单独放一个文件是因为它们本质上是数据不是逻辑 —— 混在
 * HTTP 代码里会让那个文件变得很难读。
 *
 * 三份 SCPD 声明的是这台设备会做哪些操作。**声明了就必须实现**：控制点是照 SCPD
 * 决定界面上显示哪些按钮的，声明了却做不到，用户点了没反应，比不声明还糟。
 */
namespace UpnpXml
{

/** 设备描述。控制点发现设备之后第一件事就是抓这个。 */
QString deviceDescription(const QString &friendlyName, const QString &udn);

/** AVTransport 的能力清单：播放、暂停、停止、跳转、查询状态。 */
QString avTransportScpd();

/** RenderingControl 的能力清单：音量、静音。 */
QString renderingControlScpd();

/** ConnectionManager 的能力清单：告诉控制点我们能播哪些格式。 */
QString connectionManagerScpd();

/**
 * 我们能接收的媒体格式清单（SinkProtocolInfo）。
 *
 * 放在这里是因为两个地方都要用它：HTTP 响应 GetProtocolInfo 时，以及事件推送里
 * 告诉订阅者"我的能力没变"。两处各写一份迟早会不一致。
 */
QString sinkProtocolInfo();

/** 往 XML 里塞文字之前必须转义，否则内容里一个 & 就能让整份文档非法。 */
QString escapeText(const QString &text);

/**
 * 把 XML 实体还原成普通文字。
 *
 * &lt; 必须排在 &amp; 前面处理。反过来的话，本来写成 &amp;lt; 的内容会先被还原成
 * &lt; 再被当成标签分隔符，整段就解坏了。
 */
QString unescapeText(const QString &text);

/**
 * 给定传输状态下可用的操作。
 *
 * 控制点靠这个决定界面上哪些按钮可点。**没有媒体的时候要报空** —— 报"能做 Play/
 * Pause/Stop"，控制点就会认为"片子还装着，只是停着"，于是继续维持投屏会话，
 * 手机上的界面也就一直挂着不放。
 *
 * hasNext / hasPrevious 决定要不要报出 Next / Previous —— 和前面同一个道理：
 * 没有下一条可去的时候就别报，免得控制点把按钮点亮了、用户按了却纹丝不动。
 */
QString transportActionsFor(const QString &transportState,
                            bool hasNext = false,
                            bool hasPrevious = false);

} // namespace UpnpXml
