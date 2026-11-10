// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/UpnpXml.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::size_t count(char value, const std::string &text)
{
    return static_cast<std::size_t>(
        std::count(text.begin(), text.end(), value));
}

} // namespace

int main()
{
    const std::string escaped =
        UpnpXml::escapeText("A&B <tag> \"quoted\"");
    check(
        escaped == "A&amp;B &lt;tag&gt; &quot;quoted&quot;",
        "XML escaping");
    check(
        UpnpXml::unescapeText(escaped) == "A&B <tag> \"quoted\"",
        "XML unescaping");

    const std::string description = UpnpXml::deviceDescription(
        "Living Room %2 & TV", "uuid:test");
    check(
        description.find(
            "<friendlyName>Living Room %2 &amp; TV</friendlyName>")
            != std::string::npos,
        "device description escapes and does not rescan friendly name");
    check(
        description.find("<UDN>uuid:test</UDN>") != std::string::npos,
        "device description inserts UDN");
    check(
        description.find("<friendlyName>%1") == std::string::npos
            && description.find("<UDN>%2") == std::string::npos,
        "device description template placeholders are replaced");

    check(
        UpnpXml::avTransportScpd().find("<name>SetAVTransportURI</name>")
            != std::string::npos,
        "AVTransport SCPD");
    check(
        UpnpXml::renderingControlScpd().find("<name>SetVolume</name>")
            != std::string::npos,
        "RenderingControl SCPD");
    check(
        UpnpXml::connectionManagerScpd().find("<name>GetProtocolInfo</name>")
            != std::string::npos,
        "ConnectionManager SCPD");

    const std::string sink = UpnpXml::sinkProtocolInfo();
    check(count(',', sink) == 17, "18 sink protocol entries");
    check(
        sink.find("application/dash+xml") == std::string::npos,
        "DASH is not advertised");

    check(
        UpnpXml::transportActionsFor("NO_MEDIA_PRESENT").empty(),
        "no actions without media");
    check(
        UpnpXml::transportActionsFor("PLAYING", true, false)
            == "Pause,Stop,Seek,Next",
        "playing actions");
    check(
        UpnpXml::transportActionsFor("PAUSED_PLAYBACK", false, true)
            == "Play,Stop,Seek,Previous",
        "paused actions");
    check(
        UpnpXml::transportActionsFor("TRANSITIONING", true, true)
            == "Stop",
        "transitioning actions");

    std::cout << "UPnP XML tests passed\n";
    return 0;
}
