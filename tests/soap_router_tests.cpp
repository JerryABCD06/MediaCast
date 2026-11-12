// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "protocols/dlna/UpnpXml.h"
#include "protocols/dlna/core/HttpRequest.h"
#include "protocols/dlna/net/HttpServer.h"
#include "protocols/dlna/soap/SoapRouter.h"
#include "protocols/dlna/soap/SoapXml.h"
#include "protocols/dlna/session/DlnaSession.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace mcast::dlna;
using namespace mcast::dlna::soap;
using namespace mcast::playback;

namespace {

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

mcast::dlna::core::HttpRequest request(
    std::string service, std::string action, std::string body,
    bool includeActionHeader = true)
{
    mcast::dlna::core::HttpRequest value;
    value.method = "POST";
    value.target = "/" + service + "/control";
    value.version = "HTTP/1.1";
    if (includeActionHeader) {
        value.headers.emplace_back(
            "soapaction",
            "\"urn:schemas-upnp-org:service:" + service + ":1#"
                + action + "\"");
    }
    value.body = std::move(body);
    return value;
}

std::string body(std::string action, std::string arguments = {})
{
    return "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:" + action
        + " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        + arguments + "</u:" + action + "></s:Body></s:Envelope>";
}

std::string renderingBody(
    std::string action, std::string arguments = {})
{
    return "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:" + action
        + " xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
        + arguments + "</u:" + action + "></s:Body></s:Envelope>";
}

std::string connectionBody(
    std::string action, std::string arguments = {})
{
    return "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:" + action
        + " xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
        + arguments + "</u:" + action + "></s:Body></s:Envelope>";
}

bool ok(const mcast::dlna::net::HttpResponse &response)
{
    return response.status == 200
        && response.body.find("<s:Fault>") == std::string::npos;
}

bool fault(
    const mcast::dlna::net::HttpResponse &response, int code)
{
    return response.status == 500
        && response.body.find(
               "<errorCode>" + std::to_string(code) + "</errorCode>")
            != std::string::npos;
}

void testWaitingForPlayerPlay()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    SoapRouter router(session, {
        [] { return SessionTime{1000}; },
        {},
    });
    playback.simulatePlayerLost();
    const std::string uri = "http://example.test/waiting.mp4";
    auto response = router.handle(request(
        "AVTransport", "SetAVTransportURI",
        body(
            "SetAVTransportURI",
            "<InstanceID>0</InstanceID><CurrentURI>"
                + UpnpXml::escapeText(uri)
                + "</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>")));
    check(ok(response), "open is accepted while player is unavailable");
    response = router.handle(request(
        "AVTransport", "Play",
        body("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")));
    check(ok(response),
          "Play is accepted while an open waits for the player");
    playback.simulateReconnect();
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::Playing
              && playback.snapshot().current.uri == uri,
          "waiting open and Play are replayed in order after reconnect: "
              + std::to_string(static_cast<int>(
                  playback.snapshot().state))
              + " uri=" + playback.snapshot().current.uri);
}

void testRawMetadataPassesThrough()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    SoapRouter router(session, {
        [] { return SessionTime{1000}; },
        {},
    });
    const std::string uri =
        "http://example.test/Screenshot_20260905_232916.jpg";
    const std::string didl =
        "<DIDL-Lite><item>"
        "<dc:title xmlns:dc=\"urn:test\">"
        "Screenshot_20260905_232916.jpg</dc:title>"
        "<upnp:artist xmlns:upnp=\"urn:test\">&lt;unknown&gt;</upnp:artist>"
        "<dc:creator xmlns:dc=\"urn:test\">Creator</dc:creator>"
        "<upnp:album xmlns:upnp=\"urn:test\">N/A</upnp:album>"
        "<upnp:subtitle xmlns:upnp=\"urn:test\">"
        "  A   useful\tline  </upnp:subtitle>"
        "<dc:description xmlns:dc=\"urn:test\">Description</dc:description>"
        "<upnp:albumArtURI xmlns:upnp=\"urn:test\">"
        "https://example.test/cover.jpg</upnp:albumArtURI>"
        "</item></DIDL-Lite>";
    const auto response = router.handle(request(
        "AVTransport", "SetAVTransportURI",
        body(
            "SetAVTransportURI",
            "<InstanceID>0</InstanceID><CurrentURI>"
                + UpnpXml::escapeText(uri)
                + "</CurrentURI><CurrentURIMetaData>"
                + UpnpXml::escapeText(didl)
                + "</CurrentURIMetaData>")));
    check(ok(response), "raw metadata SetURI succeeds");
    playback.processAll();
    playback.markReady();
    const auto snapshot = session.snapshot().playback;
    check(snapshot.current.title
              == "Screenshot_20260905_232916.jpg"
              && snapshot.current.artist == "<unknown>"
              && snapshot.current.creator == "Creator"
              && snapshot.current.album == "N/A",
          "protocol leaves presentation fields unfiltered");
    check(snapshot.current.subtitle.find("useful")
              != std::string::npos
              && snapshot.current.description == "Description",
          "protocol keeps subtitle candidates separate");
    check(snapshot.current.artworkUri
              == "https://example.test/cover.jpg",
          "protocol artwork URI reaches neutral media request");
    check(snapshot.current.metadata == didl,
          "network metadata keeps the original DIDL document; actual="
              + snapshot.current.metadata);
}

void testDependentCommandsWaitForAppliedState()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    SoapRouter router(session, {
        [] { return SessionTime{1000}; },
        {},
        true,
        SessionTime{2000},
    });

    MediaRequest current;
    current.uri = "http://example.test/current.mp4";
    check(session.openMedia(current, SessionTime{0}).queued,
          "dependent-command test opens current media");
    playback.processAll();
    session.peerActivity(SessionTime{1}, "192.168.10.3");
    playback.processAll();

    std::jthread setNextProcessor([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        playback.processAll();
    });
    auto response = router.handle(request(
        "AVTransport", "SetNextAVTransportURI",
        body(
            "SetNextAVTransportURI",
            "<InstanceID>0</InstanceID><NextURI>"
                "http://example.test/next.mp4</NextURI>"
                "<NextURIMetaData></NextURIMetaData>")));
    setNextProcessor.join();
    check(ok(response) && playback.snapshot().hasNext,
          "SetNext SOAP success is returned only after state is visible");

    std::jthread nextProcessor([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        playback.processAll();
    });
    response = router.handle(request(
        "AVTransport", "Next",
        body("Next", "<InstanceID>0</InstanceID>")));
    nextProcessor.join();
    check(ok(response)
              && playback.snapshot().current.uri
                  == "http://example.test/next.mp4",
          "an immediate Next sees the preceding SetNext: status="
              + std::to_string(response.status) + " current="
              + playback.snapshot().current.uri + " state="
              + std::to_string(static_cast<int>(
                  playback.snapshot().state))
              + " hasNext="
              + (playback.snapshot().hasNext ? "true" : "false")
              + " body="
              + response.body);
}

void testProtocolRetainsRawMediaWhenPlayerIsGone()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    SoapRouter router(session, {
        [] { return SessionTime{1000}; },
        {},
    });

    MediaRequest media;
    media.uri = "http://example.test/raw.mp4";
    media.metadata =
        "<DIDL-Lite><item><dc:title>Raw</dc:title></item></DIDL-Lite>";
    check(session.openMedia(media, SessionTime{0}).queued,
          "raw-record test opens media");
    playback.processAll();
    playback.markReady();
    playback.simulatePlayerLost();
    playback.processAll();

    const auto response = router.handle(request(
        "AVTransport", "GetMediaInfo",
        body("GetMediaInfo", "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find(UpnpXml::escapeText(media.uri))
                  != std::string::npos
              && response.body.find(UpnpXml::escapeText(media.metadata))
                  != std::string::npos
              && response.body.find("<NrTracks>1</NrTracks>")
                  != std::string::npos,
          "protocol answers media info from its own raw record");
}

void testDidlClassificationUsesResourceEvidence()
{
    const std::string bubbleVideo =
        "<DIDL-Lite><item>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<dc:title>studio_video</dc:title>"
        "<res protocolInfo=\"http-get:*:video/mp4:"
        "DLNA.ORG_PN=AVC_MP4\" resolution=\"2160x1080\">"
        "http://192.168.10.3/video/1000114054.mp4</res>"
        "</item></DIDL-Lite>";
    const auto video = SoapRouter::mediaRequest(
        "http://192.168.10.3/video/1000114054.mp4",
        bubbleVideo);
    check(video.kind == MediaKind::Video,
          "resource MIME and resolution override a false audioItem class");

    const std::string bubbleAudio =
        "<DIDL-Lite><item>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/flac:"
        "DLNA.ORG_OP=01\">"
        "http://192.168.10.3/audio/1000139952.flac</res>"
        "</item></DIDL-Lite>";
    const auto audio = SoapRouter::mediaRequest(
        "http://192.168.10.3/audio/1000139952.flac",
        bubbleAudio);
    check(audio.kind == MediaKind::Audio,
          "audio MIME remains authoritative for an audio resource");

    const std::string mixedResources =
        "<DIDL-Lite><item>"
        "<upnp:class>object.item.audioItem.musicTrack</upnp:class>"
        "<res protocolInfo=\"http-get:*:audio/mpeg:*\">"
        "http://192.168.10.3/audio/preview.mp3</res>"
        "<res protocolInfo=\"http-get:*:video/mp4:*\" "
        "resolution=\"3840x2160\">"
        "http://192.168.10.3/video/full.mp4</res>"
        "</item></DIDL-Lite>";
    const auto selected = SoapRouter::mediaRequest(
        "http://192.168.10.3/video/full.mp4", mixedResources);
    check(
        selected.kind == MediaKind::Video,
        "classification uses the resource matching CurrentURI");

    const std::string lanercVideo =
        "<DIDL-Lite><item>"
        "<upnp:class>object.item.videoItem</upnp:class>"
        "<res resolution=\"4\"></res>"
        "</item></DIDL-Lite>";
    const auto hls = SoapRouter::mediaRequest(
        "http://filev2.example/playlist.m3u8?auth_key=x",
        lanercVideo);
    check(hls.kind == MediaKind::Video,
          "videoItem remains a usable fallback for HLS media");
}

} // namespace

int main()
{
    testWaitingForPlayerPlay();
    testRawMetadataPassesThrough();
    testDependentCommandsWaitForAppliedState();
    testProtocolRetainsRawMediaWhenPlayerIsGone();
    testDidlClassificationUsesResourceEvidence();
    FakePlaybackGateway playback;
    playback.setPictureControls({
        {"brightness", -100, 100, 0, 0},
        {"contrast", -100, 100, 0, 0},
        {"sharpen", -100, 100, 0, 0},
    });
    DlnaSession session(playback, {});
    std::vector<std::string> logs;
    SoapRouter router(session, {
        [] { return SessionTime{1000}; },
        [&](const std::string &message) { logs.push_back(message); },
    });

    SoapRequestXml parsedXml;
    std::string xmlError;
    const std::string xmlWithComment =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:Play xmlns:u=\"urn:schemas-upnp-org:service:"
        "AVTransport:1\"><!-- <InstanceID>1</InstanceID> -->"
        "<InstanceID>0</InstanceID><Speed><![CDATA[1]]></Speed>"
        "</u:Play></s:Body></s:Envelope>";
    check(
        parseSoapRequest(
            xmlWithComment, "AVTransport", parsedXml, xmlError)
            && parsedXml.action == "Play"
            && parsedXml.value("InstanceID").value_or("") == "0"
            && parsedXml.value("Speed").value_or("") == "1",
        "XmlLite ignores comments and decodes CDATA");

    DidlMetadata parsedDidl;
    const std::string metadataWithEntities =
        "<DIDL-Lite><item><dc:title xmlns:dc=\"urn:test\">"
        "A &#38; B</dc:title><upnp:description xmlns:upnp=\"urn:test\">"
        "<![CDATA[Hello <world>]]></upnp:description>"
        "</item></DIDL-Lite>";
    check(
        parseDidlMetadata(
            metadataWithEntities, parsedDidl, xmlError)
            && parsedDidl.title.value_or("") == "A & B"
            && parsedDidl.description.value_or("") == "Hello <world>",
        "DIDL parser supports numeric entities and CDATA");

    const std::string forbiddenDtd =
        "<!DOCTYPE x [<!ENTITY e \"boom\">]>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:Play xmlns:u=\"urn:schemas-upnp-org:service:"
        "AVTransport:1\"><InstanceID>0</InstanceID></u:Play>"
        "</s:Body></s:Envelope>";
    check(!parseSoapRequest(
              forbiddenDtd, "AVTransport", parsedXml, xmlError),
          "DTD in SOAP XML is rejected");

    const std::string actionOutsideBody =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body/>"
        "<Other><u:GetProtocolInfo xmlns:u=\"urn:schemas-upnp-org:"
        "service:ConnectionManager:1\"/></Other>"
        "</s:Envelope>";
    check(!parseSoapRequest(
              actionOutsideBody, "ConnectionManager",
              parsedXml, xmlError),
          "SOAP action outside Body is rejected");

    auto response = router.handle(request(
        "ConnectionManager", "GetProtocolInfo",
        connectionBody("GetProtocolInfo")));
    check(ok(response) && response.body.find("<Sink>")
              != std::string::npos,
          "GetProtocolInfo responds with sink protocols: "
              + std::to_string(response.status) + " " + response.body);

    response = router.handle(request(
        "AVTransport", "Play",
        body("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")));
    check(
        fault(response, 701),
        "Play without media returns 701: "
            + std::to_string(response.status) + " " + response.body
            + " logs=" + [&] {
                  std::string joined;
                  for (const auto &line : logs)
                      joined += line + " | ";
                  return joined;
              }());

    auto mismatched = request(
        "AVTransport", "Play",
        body("Stop", "<InstanceID>0</InstanceID>"));
    response = router.handle(mismatched);
    check(fault(response, 401),
          "SOAPAction and body action mismatch is rejected");

    auto badNamespace = request(
        "AVTransport", "Play",
        body("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>"));
    badNamespace.headers.front().second = "\"urn:evil#Play\"";
    response = router.handle(badNamespace);
    check(fault(response, 401),
          "SOAPAction service namespace mismatch is rejected");

    auto wrongPath = request(
        "AVTransport", "GetTransportInfo",
        body("GetTransportInfo", "<InstanceID>0</InstanceID>"));
    wrongPath.target = "/AVTransport/extra/control";
    response = router.handle(wrongPath);
    check(fault(response, 401), "extra control path segments are rejected");

    response = router.handle(request(
        "AVTransport", "Play", body("Play", "<Speed>1</Speed>")));
    check(fault(response, 402), "missing InstanceID returns 402");

    response = router.handle(request(
        "AVTransport", "GetTransportInfo",
        body("GetTransportInfo", "<InstanceID>1</InstanceID>")));
    check(fault(response, 718), "invalid InstanceID returns 718");

    const std::string uri = "http://example.test/video.mp4?a=1&b=2";
    const std::string didl =
        "<DIDL-Lite><item><dc:title xmlns:dc=\"urn:test\">"
        "My &amp; Movie</dc:title>"
        "<upnp:class xmlns:upnp=\"urn:test\">"
        "object.item.videoItem</upnp:class>"
        "</item></DIDL-Lite>";
    const std::string setUriBody = body(
        "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>"
            + UpnpXml::escapeText(uri)
            + "</CurrentURI><CurrentURIMetaData>"
            + UpnpXml::escapeText(didl)
            + "</CurrentURIMetaData>");
    response = router.handle(request(
        "AVTransport", "SetAVTransportURI", setUriBody, false));
    check(ok(response)
              && response.body.find(
                  "<u:SetAVTransportURIResponse")
                  != std::string::npos,
          "SetAVTransportURI action is inferred from body");
    response = router.handle(request(
        "AVTransport", "Play",
        body("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")));
    check(ok(response),
          "Play immediately follows a queued Open");
    playback.processAll();
    PlaybackSnapshot snapshot = playback.snapshot();
    check(snapshot.state == PlaybackState::Playing
              && snapshot.current.uri == uri
              && snapshot.current.title == "My & Movie"
              && snapshot.current.kind == MediaKind::Video
              && snapshot.current.metadata.empty(),
          "SetAVTransportURI maps to neutral Open command");
    check(session.snapshot().playback.current.metadata == didl,
          "Protocol retains the raw DIDL outside the Ui command");

    playback.markReady();
    response = router.handle(request(
        "AVTransport", "GetTransportInfo",
        body("GetTransportInfo", "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find(
                  "<CurrentTransportState>PLAYING</CurrentTransportState>")
                  != std::string::npos,
          "GetTransportInfo reads playback snapshot");

    response = router.handle(request(
        "AVTransport", "GetTransportSettings",
        body("GetTransportSettings", "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find("<PlayMode>NORMAL</PlayMode>")
                  != std::string::npos
              && response.body.find("<RecMediaDuration>00:00:00")
                  != std::string::npos,
          "GetTransportSettings reads play mode");

    response = router.handle(request(
        "AVTransport", "GetMediaInfo",
        body("GetMediaInfo", "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find(UpnpXml::escapeText(uri))
                  != std::string::npos
              && response.body.find("<NrTracks>1</NrTracks>")
                  != std::string::npos,
          "GetMediaInfo reads current/next media");

    response = router.handle(request(
        "AVTransport", "GetPositionInfo",
        body("GetPositionInfo", "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find("<RelTime>0:00:00</RelTime>")
                  != std::string::npos,
          "GetPositionInfo serializes UPnP time");

    const std::string nextUri = "http://example.test/next.mp4";
    response = router.handle(request(
        "AVTransport", "SetNextAVTransportURI",
        body(
            "SetNextAVTransportURI",
            "<InstanceID>0</InstanceID><NextURI>"
                + UpnpXml::escapeText(nextUri)
                + "</NextURI><NextURIMetaData></NextURIMetaData>")));
    check(ok(response), "SetNextAVTransportURI succeeds");
    playback.processAll();
    check(playback.snapshot().next.uri == nextUri,
          "SetNextAVTransportURI maps to SetNext command");

    response = router.handle(request(
        "AVTransport", "GetCurrentTransportActions",
        body(
            "GetCurrentTransportActions",
            "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find("<Actions>")
                  != std::string::npos
              && response.body.find("Next") != std::string::npos,
          "transport actions reflect available queue");

    response = router.handle(request(
        "AVTransport", "Seek",
        body(
            "Seek",
            "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit>"
            "<Target>0:01:30</Target>")));
    check(ok(response), "REL_TIME seek succeeds");
    playback.processAll();
    check(std::abs(playback.snapshot().positionSeconds - 90.0) < 0.01,
          "seek maps to neutral seconds");

    response = router.handle(request(
        "AVTransport", "Pause",
        body("Pause", "<InstanceID>0</InstanceID>")));
    check(ok(response), "Pause succeeds");
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::Paused,
          "Pause maps to Playback state");
    response = router.handle(request(
        "AVTransport", "Play",
        body("Play", "<InstanceID>0</InstanceID><Speed>2</Speed>")));
    check(fault(response, 717), "unsupported play speed returns 717");
    response = router.handle(request(
        "AVTransport", "Play",
        body("Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")));
    check(ok(response), "Play resumes after pause");
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::Playing,
          "Play maps to Playback state");

    response = router.handle(request(
        "AVTransport", "SetPlayMode",
        body(
            "SetPlayMode",
            "<InstanceID>0</InstanceID><NewPlayMode>REPEAT_ALL"
            "</NewPlayMode>")));
    check(ok(response), "SetPlayMode succeeds");
    playback.processAll();
    check(playback.snapshot().playMode == PlayMode::RepeatAll,
          "play mode maps to neutral enum");

    response = router.handle(request(
        "RenderingControl", "SetVolume",
        renderingBody(
            "SetVolume",
            "<InstanceID>0</InstanceID><Channel>Master</Channel>"
            "<DesiredVolume>55</DesiredVolume>")));
    check(ok(response), "SetVolume succeeds");
    playback.processAll();
    check(playback.snapshot().volumePercent == 55,
          "volume maps to Playback SetVolume");

    response = router.handle(request(
        "RenderingControl", "GetVolume",
        renderingBody(
            "GetVolume",
            "<InstanceID>0</InstanceID><Channel>Master</Channel>")));
    check(ok(response)
              && response.body.find("<CurrentVolume>55</CurrentVolume>")
                  != std::string::npos,
          "GetVolume reads snapshot");
    response = router.handle(request(
        "RenderingControl", "SetVolume",
        renderingBody(
            "SetVolume",
            "<InstanceID>0</InstanceID><Channel>Master</Channel>"
            "<DesiredVolume>101"
            "</DesiredVolume>")));
    check(fault(response, 402), "out-of-range volume returns 402");

    response = router.handle(request(
        "RenderingControl", "SetMute",
        renderingBody(
            "SetMute",
            "<InstanceID>0</InstanceID><Channel>Master</Channel>"
            "<DesiredMute>true</DesiredMute>")));
    check(ok(response)
              && response.body.find("<u:SetMuteResponse")
                  != std::string::npos,
          "SetMute succeeds with the correct response action");
    playback.processAll();
    check(playback.snapshot().muted, "mute maps to Playback state");
    response = router.handle(request(
        "RenderingControl", "SetMute",
        renderingBody(
            "SetMute",
            "<InstanceID>0</InstanceID><Channel>Master</Channel>"
            "<DesiredMute>bogus</DesiredMute>")));
    check(fault(response, 402), "invalid Mute value returns 402");
    response = router.handle(request(
        "RenderingControl", "GetMute",
        renderingBody(
            "GetMute",
            "<InstanceID>0</InstanceID><Channel>Master</Channel>")));
    check(ok(response)
              && response.body.find("<CurrentMute>1</CurrentMute>")
                  != std::string::npos,
          "GetMute reads snapshot");

    response = router.handle(request(
        "RenderingControl", "SetBrightness",
        renderingBody(
            "SetBrightness",
            "<InstanceID>0</InstanceID><DesiredBrightness>75"
            "</DesiredBrightness>")));
    check(ok(response), "SetBrightness succeeds");
    playback.processAll();
    check(playback.snapshot().pictureControls[0].value == 50,
          "DLNA picture value maps to backend range");
    response = router.handle(request(
        "RenderingControl", "GetBrightness",
        renderingBody(
            "GetBrightness",
            "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find(
                  "<CurrentBrightness>75</CurrentBrightness>")
                  != std::string::npos,
          "GetBrightness converts backend value");
    response = router.handle(request(
        "RenderingControl", "ListPresets",
        renderingBody(
            "ListPresets",
            "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find("FactoryDefaults")
                  != std::string::npos,
          "ListPresets reports factory defaults");
    response = router.handle(request(
        "RenderingControl", "SelectPreset",
        renderingBody(
            "SelectPreset",
            "<InstanceID>0</InstanceID>"
            "<PresetName>FactoryDefaults</PresetName>")));
    check(ok(response), "SelectPreset resets picture controls");
    playback.processAll();
    check(playback.snapshot().pictureControls[0].value == 0,
          "factory preset restores backend neutral");

    response = router.handle(request(
        "AVTransport", "GetDeviceCapabilities",
        body(
            "GetDeviceCapabilities",
            "<InstanceID>0</InstanceID>")));
    check(ok(response)
              && response.body.find("<PlayMedia>NETWORK</PlayMedia>")
                  != std::string::npos,
          "GetDeviceCapabilities reports network media");

    response = router.handle(request(
        "ConnectionManager", "GetCurrentConnectionIDs",
        connectionBody("GetCurrentConnectionIDs")));
    check(ok(response)
              && response.body.find(
                  "<ConnectionIDs>0</ConnectionIDs>")
                  != std::string::npos,
          "active media exposes connection zero");

    response = router.handle(request(
        "ConnectionManager", "GetCurrentConnectionInfo",
        connectionBody(
            "GetCurrentConnectionInfo",
            "<ConnectionID>0</ConnectionID>")));
    check(ok(response)
              && response.body.find("<Direction>Input</Direction>")
                  != std::string::npos,
          "connection info describes renderer input");

    response = router.handle(request(
        "AVTransport", "Next",
        body("Next", "<InstanceID>0</InstanceID>")));
    check(ok(response), "Next succeeds");
    playback.processAll();
    check(playback.snapshot().current.uri == nextUri,
          "Next maps to Playback command");
    response = router.handle(request(
        "AVTransport", "Previous",
        body("Previous", "<InstanceID>0</InstanceID>")));
    check(ok(response), "Previous succeeds");
    playback.processAll();
    check(playback.snapshot().current.uri == uri,
          "Previous maps to Playback command");
    response = router.handle(request(
        "AVTransport", "Stop",
        body("Stop", "<InstanceID>0</InstanceID>")));
    check(ok(response), "Stop succeeds");
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::Stopped,
          "Stop maps to Playback state");
    session.tick(SessionTime{8999});
    check(session.snapshot().peerConnected,
          "Stop keeps the control point during switch grace");
    session.tick(SessionTime{9000});
    check(session.snapshot().peerConnected,
          "Stop boundary settles before ending the control-point session");
    session.tick(SessionTime{9250});
    playback.processAll();
    check(!session.snapshot().peerConnected
              && playback.snapshot().state == PlaybackState::NoMedia,
          "idle Stop ends the control-point session");

    response = router.handle(request(
        "AVTransport", "Bogus", body("Bogus")));
    check(fault(response, 401), "unknown action returns 401");
    check(!logs.empty(), "router logs handled actions");

    std::cout << "SOAP router tests passed\n";
    return 0;
}
