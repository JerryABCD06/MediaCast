// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/soap/SoapRouter.h"

#include "protocols/dlna/UpnpXml.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace mcast::dlna::soap {
namespace {

constexpr int kInvalidAction = 401;
constexpr int kInvalidArgs = 402;
constexpr int kActionFailed = 501;
constexpr int kInvalidConnectionReference = 706;
constexpr int kTransitionNotAvailable = 701;
constexpr int kInvalidName = 701;
constexpr int kSeekModeNotSupported = 710;
constexpr int kIllegalSeekTarget = 711;
constexpr int kPlaySpeedNotSupported = 717;
constexpr int kInvalidInstanceId = 718;

SessionTime steadyNow()
{
    return std::chrono::duration_cast<SessionTime>(
        std::chrono::steady_clock::now().time_since_epoch());
}

std::string trim(std::string_view value)
{
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return std::string(value);
}

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return result;
}

std::optional<int> parseInt(std::string_view value)
{
    const std::string text = trim(value);
    value = text;
    int parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{}
        || result.ptr != value.data() + value.size())
        return std::nullopt;
    return parsed;
}

std::optional<std::uint64_t> parseUInt64(std::string_view value)
{
    const std::string text = trim(value);
    value = text;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{}
        || result.ptr != value.data() + value.size())
        return std::nullopt;
    return parsed;
}

std::optional<double> parseDouble(std::string_view value)
{
    const std::string text = trim(value);
    value = text;
    double parsed = 0.0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{}
        || result.ptr != value.data() + value.size()
        || !std::isfinite(parsed))
        return std::nullopt;
    return parsed;
}

std::optional<double> parseUpnpTime(std::string_view value)
{
    const std::string text = trim(value);
    value = text;
    if (value.empty())
        return std::nullopt;
    double result = 0.0;
    std::size_t offset = 0;
    while (offset <= value.size()) {
        const std::size_t colon = value.find(':', offset);
        const std::string_view part = value.substr(
            offset, colon == std::string_view::npos
                ? value.size() - offset : colon - offset);
        const auto parsed = parseDouble(part);
        if (!parsed || *parsed < 0.0)
            return std::nullopt;
        result = result * 60.0 + *parsed;
        if (colon == std::string_view::npos)
            break;
        offset = colon + 1;
    }
    return result;
}

std::string upnpTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;
    const std::uint64_t total =
        static_cast<std::uint64_t>(seconds);
    std::ostringstream output;
    output << total / 3600 << ':' << std::setfill('0')
           << std::setw(2) << (total % 3600) / 60 << ':'
           << std::setw(2) << total % 60;
    return output.str();
}

std::string dlnaState(playback::PlaybackState state)
{
    switch (state) {
    case playback::PlaybackState::NoMedia:
        return "NO_MEDIA_PRESENT";
    case playback::PlaybackState::Stopped:
        return "STOPPED";
    case playback::PlaybackState::Preparing:
        return "TRANSITIONING";
    case playback::PlaybackState::Playing:
        return "PLAYING";
    case playback::PlaybackState::Paused:
        return "PAUSED_PLAYBACK";
    }
    return "NO_MEDIA_PRESENT";
}

std::string dlnaPlayMode(playback::PlayMode mode)
{
    switch (mode) {
    case playback::PlayMode::Normal:
        return "NORMAL";
    case playback::PlayMode::RepeatOne:
        return "REPEAT_ONE";
    case playback::PlayMode::RepeatAll:
        return "REPEAT_ALL";
    case playback::PlayMode::Direct:
        return "DIRECT_1";
    }
    return "NORMAL";
}

std::optional<playback::PlayMode> parsePlayMode(std::string_view value)
{
    const std::string mode = lower(trim(value));
    if (mode == "normal")
        return playback::PlayMode::Normal;
    if (mode == "repeat_one")
        return playback::PlayMode::RepeatOne;
    if (mode == "repeat_all")
        return playback::PlayMode::RepeatAll;
    if (mode == "direct_1")
        return playback::PlayMode::Direct;
    return std::nullopt;
}

playback::MediaRequest parseMediaRequest(
    std::string uri, std::string metadata)
{
    playback::MediaRequest request;
    request.uri = std::move(uri);
    request.metadata = std::move(metadata);
    request.protocol = "DLNA";
    request.source = playback::MediaSource::Cast;

    DidlMetadata didl;
    std::string parseError;
    parseDidlMetadata(request.metadata, didl, parseError);
    if (didl.upnpClass) {
        if (didl.upnpClass->find("object.item.imageItem")
            != std::string::npos) {
            request.kind = playback::MediaKind::Image;
        } else if (didl.upnpClass->find("object.item.audioItem")
                   != std::string::npos) {
            request.kind = playback::MediaKind::Audio;
        } else if (didl.upnpClass->find("object.item.videoItem")
                   != std::string::npos) {
            request.kind = playback::MediaKind::Video;
        }
    }
    request.title = didl.title.value_or("");
    request.artist = didl.artist.value_or(
        didl.creator.value_or(""));
    request.album = didl.album.value_or("");
    if (didl.subtitle)
        request.description = *didl.subtitle;
    else if (didl.description)
        request.description = *didl.description;
    else if (didl.longDescription)
        request.description = *didl.longDescription;
    return request;
}

HttpResponse soapResponse(
    const std::string &service, const std::string &action,
    const std::string &inner = {})
{
    HttpResponse response;
    response.contentType = "text/xml; charset=\"utf-8\"";
    response.body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "<s:Body><u:" + action
        + "Response xmlns:u=\"urn:schemas-upnp-org:service:"
        + service + ":1\">" + inner + "</u:" + action
        + "Response></s:Body>\r\n</s:Envelope>\r\n";
    return response;
}

HttpResponse soapFault(int code, std::string_view text)
{
    HttpResponse response;
    response.status = 500;
    response.contentType = "text/xml; charset=\"utf-8\"";
    response.body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "<s:Body><s:Fault>\r\n"
        "<faultcode>s:Client</faultcode>"
        "<faultstring>UPnPError</faultstring>\r\n"
        "<detail><UPnPError "
        "xmlns=\"urn:schemas-upnp-org:control-1-0\">\r\n"
        "<errorCode>" + std::to_string(code)
        + "</errorCode><errorString>" + std::string(text)
        + "</errorString>\r\n</UPnPError></detail>\r\n"
        "</s:Fault></s:Body>\r\n</s:Envelope>\r\n";
    return response;
}

std::string escaped(std::string_view value)
{
    return UpnpXml::escapeText(std::string(value));
}

bool knownAction(
    std::string_view service, std::string_view action)
{
    static constexpr std::string_view avTransportActions[] = {
        "SetAVTransportURI",
        "SetNextAVTransportURI",
        "Play",
        "Pause",
        "Stop",
        "Seek",
        "Next",
        "Previous",
        "SetPlayMode",
        "GetTransportInfo",
        "GetPositionInfo",
        "GetMediaInfo",
        "GetCurrentTransportActions",
        "GetTransportSettings",
        "GetDeviceCapabilities",
    };
    static constexpr std::string_view renderingActions[] = {
        "SetVolume", "GetVolume", "SetMute", "GetMute",
        "SetBrightness", "GetBrightness", "SetContrast",
        "GetContrast", "SetSharpness", "GetSharpness",
        "ListPresets", "SelectPreset",
    };
    static constexpr std::string_view connectionActions[] = {
        "GetProtocolInfo", "GetCurrentConnectionIDs",
        "GetCurrentConnectionInfo",
    };
    const auto contains = [&](const auto &actions) {
        return std::find(
                   std::begin(actions), std::end(actions), action)
            != std::end(actions);
    };
    if (service == "AVTransport")
        return contains(avTransportActions);
    if (service == "RenderingControl")
        return contains(renderingActions);
    if (service == "ConnectionManager")
        return contains(connectionActions);
    return false;
}

} // namespace

SoapRouter::SoapRouter(
    DlnaSession &session, SoapRouterOptions options)
    : m_session(session)
    , m_options(std::move(options))
{
    if (!m_options.now)
        m_options.now = steadyNow;
}

HttpResponse SoapRouter::handle(const core::HttpRequest &request)
{
    HttpResponse errorResponse;
    const auto parsed = parseRequest(request, errorResponse);
    if (!parsed)
        return errorResponse;
    return handleParsed(*parsed);
}

std::optional<SoapRequestContext> SoapRouter::parseRequest(
    const core::HttpRequest &request,
    HttpResponse &errorResponse)
{
    if (request.method != "POST")
        return errorResponse =
            soapFault(kInvalidAction, "Invalid Action"), std::nullopt;

    std::string target = request.target;
    const std::size_t query = target.find('?');
    if (query != std::string::npos)
        target.erase(query);
    std::string service;
    if (target == "/AVTransport/control")
        service = "AVTransport";
    else if (target == "/RenderingControl/control")
        service = "RenderingControl";
    else if (target == "/ConnectionManager/control")
        service = "ConnectionManager";
    else
        return errorResponse =
            soapFault(kInvalidAction, "Invalid Action"), std::nullopt;

    SoapRequestXml body;
    std::string parseError;
    if (!parseSoapRequest(request.body, service, body, parseError)
        || !knownAction(service, body.action)) {
        log("SOAP XML rejected: " + parseError);
        return errorResponse =
            soapFault(kInvalidAction, "Invalid Action"), std::nullopt;
    }

    const std::string expectedNamespace =
        "urn:schemas-upnp-org:service:" + service + ":1";
    std::string action = body.action;
    if (const auto soapAction = request.header("soapaction")) {
        std::string value = trim(*soapAction);
        if (value.size() >= 2 && value.front() == '"'
            && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        const std::size_t hash = value.rfind('#');
        if (hash == std::string::npos) {
            log("SOAPAction has no service/action separator");
            return errorResponse =
                soapFault(kInvalidAction, "Invalid Action"), std::nullopt;
        }
        const std::string actionNamespace = value.substr(0, hash);
        const std::string headerAction = value.substr(hash + 1);
        if (actionNamespace != expectedNamespace
            || headerAction != body.action) {
            log("SOAPAction does not match path and body");
            return errorResponse =
                soapFault(kInvalidAction, "Invalid Action"), std::nullopt;
        }
        action = headerAction;
    }

    if (service != "ConnectionManager") {
        const auto instance = body.value("InstanceID");
        if (!instance)
            return errorResponse =
                soapFault(kInvalidArgs, "Invalid Args"), std::nullopt;
        const auto id = parseInt(*instance);
        if (!id || *id != 0)
            return errorResponse = soapFault(
                kInvalidInstanceId, "Invalid InstanceID"), std::nullopt;
    }

    log("SOAP " + service + "#" + action);
    return SoapRequestContext{
        std::move(service),
        std::move(action),
        std::move(body),
    };
}

HttpResponse SoapRouter::handleParsed(
    const SoapRequestContext &request)
{
    const HttpResponse response = dispatch(
        request.service, request.action, request.body);
    if (request.service == "AVTransport"
        && request.action == "SetAVTransportURI") {
        noteNewMediaRequest();
    } else {
        notePeerActivity();
    }
    return response;
}

void SoapRouter::notePeerActivity()
{
    m_session.peerActivity(m_options.now());
}

void SoapRouter::noteNewMediaRequest()
{
    m_session.newMediaRequested(m_options.now());
}

playback::MediaRequest SoapRouter::mediaRequest(
    std::string uri, std::string metadata)
{
    return parseMediaRequest(std::move(uri), std::move(metadata));
}

HttpResponse SoapRouter::successResponse(
    const std::string &service, const std::string &action)
{
    return soapResponse(service, action);
}

HttpResponse SoapRouter::faultResponse(
    int code, std::string_view text)
{
    return soapFault(code, text);
}

HttpResponse SoapRouter::dispatch(
    const std::string &service, const std::string &action,
    const SoapRequestXml &body)
{
    if (service == "AVTransport")
        return handleAvTransport(action, body);
    if (service == "RenderingControl")
        return handleRenderingControl(action, body);
    if (service == "ConnectionManager")
        return handleConnectionManager(action, body);
    return soapFault(kInvalidAction, "Invalid Action");
}

HttpResponse SoapRouter::handleAvTransport(
    const std::string &action, const SoapRequestXml &body)
{
    const playback::PlaybackSnapshot snapshot =
        m_session.snapshot().playback;

    if (action == "SetAVTransportURI") {
        const auto uriValue = body.value("CurrentURI");
        const auto metadataValue = body.value("CurrentURIMetaData");
        if (!uriValue || !metadataValue)
            return soapFault(kInvalidArgs, "Invalid Args");
        const std::string uri = *uriValue;
        const std::string metadata = *metadataValue;
        auto media = mediaRequest(uri, metadata);
        if (media.uri.empty())
            return soapFault(kInvalidArgs, "Invalid Args");
        return submitMedia(std::move(media), "AVTransport", action);
    }

    if (action == "SetNextAVTransportURI") {
        const auto uriValue = body.value("NextURI");
        const auto metadataValue = body.value("NextURIMetaData");
        if (!uriValue || !metadataValue)
            return soapFault(kInvalidArgs, "Invalid Args");
        const std::string uri = *uriValue;
        const std::string metadata = *metadataValue;
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::SetNext;
        command.media = mediaRequest(uri, metadata);
        return submit(std::move(command), "AVTransport", action);
    }

    if ((action == "Next" && !snapshot.hasNext)
        || (action == "Previous" && !snapshot.hasPrevious)) {
        return soapFault(
            kTransitionNotAvailable, "Transition not available");
    }

    if (snapshot.state == playback::PlaybackState::NoMedia
        && !m_session.transportTransitionPending()
        && (action == "Play" || action == "Pause"
            || action == "Stop" || action == "Seek"
            || action == "Next" || action == "Previous")) {
        return soapFault(
            kTransitionNotAvailable, "Transition not available");
    }

    if (action == "Play") {
        const auto speedValue = body.value("Speed");
        if (!speedValue)
            return soapFault(kInvalidArgs, "Invalid Args");
        const std::string speed = *speedValue;
        if (speed != "1"
            && speed != "1.0" && speed != "1/1") {
            return soapFault(
                kPlaySpeedNotSupported,
                "Play speed not supported");
        }
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::Play;
        return submit(std::move(command), "AVTransport", action);
    }
    if (action == "Pause") {
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::Pause;
        return submit(std::move(command), "AVTransport", action);
    }
    if (action == "Stop") {
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::Stop;
        return submit(std::move(command), "AVTransport", action);
    }
    if (action == "Next" || action == "Previous") {
        playback::PlaybackCommand command;
        command.type = action == "Next"
            ? playback::PlaybackCommandType::Next
            : playback::PlaybackCommandType::Previous;
        return submit(std::move(command), "AVTransport", action);
    }
    if (action == "Seek") {
        const auto unitValue = body.value("Unit");
        const auto targetValue = body.value("Target");
        if (!unitValue || !targetValue)
            return soapFault(kInvalidArgs, "Invalid Args");
        const std::string unit = lower(*unitValue);
        const std::string target = *targetValue;
        if (unit.empty())
            return soapFault(kInvalidArgs, "Invalid Args");
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::Seek;
        if (unit == "rel_time") {
            const auto seconds = parseUpnpTime(target);
            if (!seconds)
                return soapFault(kIllegalSeekTarget, "Illegal seek target");
            command.doubleValue = *seconds;
        } else if (unit == "track_nr") {
            const auto track = parseInt(target);
            if (!track || *track != 1)
                return soapFault(kIllegalSeekTarget, "Illegal seek target");
            command.doubleValue = 0.0;
        } else if (unit == "x_dlna_rel_byte") {
            const auto byte = parseUInt64(target);
            if (!byte
                || !snapshot.current.mediaSizeBytes
                || *snapshot.current.mediaSizeBytes == 0
                || snapshot.durationSeconds <= 0.0) {
                return soapFault(kIllegalSeekTarget, "Illegal seek target");
            }
            command.doubleValue = snapshot.durationSeconds
                * (static_cast<double>(*byte)
                   / static_cast<double>(*snapshot.current.mediaSizeBytes));
        } else {
            return soapFault(
                kSeekModeNotSupported, "Seek mode not supported");
        }
        return submit(std::move(command), "AVTransport", action);
    }
    if (action == "SetPlayMode") {
        const auto modeValue = body.value("NewPlayMode");
        if (!modeValue)
            return soapFault(kInvalidArgs, "Invalid Args");
        const auto mode = parsePlayMode(*modeValue);
        if (!mode)
            return soapFault(
                kTransitionNotAvailable, "Transition not available");
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::SetPlayMode;
        command.playMode = *mode;
        return submit(std::move(command), "AVTransport", action);
    }
    if (action == "GetTransportSettings") {
        return soapResponse(
            "AVTransport", action,
            "<PlayMode>" + dlnaPlayMode(snapshot.playMode)
                + "</PlayMode><RecMediaDuration>00:00:00"
                  "</RecMediaDuration>");
    }
    if (action == "GetDeviceCapabilities") {
        return soapResponse(
            "AVTransport", action,
            "<PlayMedia>NETWORK</PlayMedia>"
            "<RecMedia>NOT_IMPLEMENTED</RecMedia>"
            "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
    }
    if (action == "GetTransportInfo") {
        return soapResponse(
            "AVTransport", action,
            "<CurrentTransportState>" + dlnaState(snapshot.state)
                + "</CurrentTransportState><CurrentTransportStatus>"
                + (snapshot.loadStatus == playback::LoadStatus::Failed
                       ? "ERROR_OCCURRED" : "OK")
                + "</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>");
    }
    if (action == "GetPositionInfo") {
        const std::string position = upnpTime(snapshot.positionSeconds);
        return soapResponse(
            "AVTransport", action,
            "<Track>1</Track><TrackDuration>"
                + upnpTime(snapshot.durationSeconds)
                + "</TrackDuration><TrackMetaData>"
                + escaped(snapshot.current.metadata)
                + "</TrackMetaData><TrackURI>"
                + escaped(snapshot.current.uri)
                + "</TrackURI><RelTime>" + position
                + "</RelTime><AbsTime>" + position
                + "</AbsTime><RelCount>2147483647</RelCount>"
                  "<AbsCount>2147483647</AbsCount>");
    }
    if (action == "GetMediaInfo") {
        const bool hasMedia =
            snapshot.state != playback::PlaybackState::NoMedia;
        return soapResponse(
            "AVTransport", action,
            "<NrTracks>" + std::to_string(hasMedia ? 1 : 0)
                + "</NrTracks><MediaDuration>"
                + upnpTime(snapshot.durationSeconds)
                + "</MediaDuration><CurrentURI>"
                + escaped(snapshot.current.uri)
                + "</CurrentURI><CurrentURIMetaData>"
                + escaped(snapshot.current.metadata)
                + "</CurrentURIMetaData><NextURI>"
                + escaped(snapshot.next.uri)
                + "</NextURI><NextURIMetaData>"
                + escaped(snapshot.next.metadata)
                + "</NextURIMetaData><PlayMedium>NETWORK</PlayMedium>"
                  "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                  "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
    }
    if (action == "GetCurrentTransportActions") {
        return soapResponse(
            "AVTransport", action,
            "<Actions>"
                + UpnpXml::transportActionsFor(
                    dlnaState(snapshot.state), snapshot.hasNext,
                    snapshot.hasPrevious)
                + "</Actions>");
    }
    return soapFault(kInvalidAction, "Invalid Action");
}

HttpResponse SoapRouter::handleRenderingControl(
    const std::string &action, const SoapRequestXml &body)
{
    const playback::PlaybackSnapshot snapshot =
        m_session.snapshot().playback;
    if (action == "SetVolume") {
        const auto channel = body.value("Channel");
        const auto volume = parseInt(body.value("DesiredVolume").value_or(""));
        if (!channel || lower(*channel) != "master"
            || !volume || *volume < 0 || *volume > 100)
            return soapFault(kInvalidArgs, "Invalid Args");
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::SetVolume;
        command.intValue = *volume;
        return submit(std::move(command), "RenderingControl", action);
    }
    if (action == "GetVolume") {
        const auto channel = body.value("Channel");
        if (!channel || lower(*channel) != "master")
            return soapFault(kInvalidArgs, "Invalid Args");
        return soapResponse(
            "RenderingControl", action,
            "<CurrentVolume>" + std::to_string(snapshot.volumePercent)
                + "</CurrentVolume>");
    }
    if (action == "SetMute") {
        const auto channel = body.value("Channel");
        const auto desired = body.value("DesiredMute");
        if (!channel || lower(*channel) != "master" || !desired)
            return soapFault(kInvalidArgs, "Invalid Args");
        const std::string value = lower(*desired);
        if (value != "0" && value != "1"
            && value != "false" && value != "true") {
            return soapFault(kInvalidArgs, "Invalid Args");
        }
        playback::PlaybackCommand command;
        command.type = playback::PlaybackCommandType::SetMuted;
        command.boolValue = value == "1" || value == "true";
        return submit(std::move(command), "RenderingControl", action);
    }
    if (action == "GetMute") {
        const auto channel = body.value("Channel");
        if (!channel || lower(*channel) != "master")
            return soapFault(kInvalidArgs, "Invalid Args");
        return soapResponse(
            "RenderingControl", action,
            "<CurrentMute>"
                + std::string(snapshot.muted ? "1" : "0")
                + "</CurrentMute>");
    }

    struct PictureAction {
        std::string_view dlna;
        std::string_view control;
    };
    constexpr PictureAction pictures[] = {
        {"Brightness", "brightness"},
        {"Contrast", "contrast"},
        {"Sharpness", "sharpen"},
    };
    for (const auto &picture : pictures) {
        const auto found = std::find_if(
            snapshot.pictureControls.begin(),
            snapshot.pictureControls.end(),
            [&](const playback::PictureControl &control) {
                return control.name == picture.control;
            });
        const std::string getAction =
            "Get" + std::string(picture.dlna);
        const std::string setAction =
            "Set" + std::string(picture.dlna);
        if (action == getAction) {
            const int raw = found == snapshot.pictureControls.end()
                ? 0 : found->value;
            const int dlna = std::clamp(raw / 2 + 50, 0, 100);
            return soapResponse(
                "RenderingControl", action,
                "<Current" + std::string(picture.dlna) + ">"
                    + std::to_string(dlna) + "</Current"
                    + std::string(picture.dlna) + ">");
        }
        if (action == setAction) {
            const auto desired = body.value(
                "Desired" + std::string(picture.dlna));
            if (!desired)
                return soapFault(kInvalidArgs, "Invalid Args");
            const auto value = parseInt(*desired);
            if (!value || *value < 0 || *value > 100
                || found == snapshot.pictureControls.end()) {
                return soapFault(kInvalidArgs, "Invalid Args");
            }
            playback::PlaybackCommand command;
            command.type =
                playback::PlaybackCommandType::SetPictureControl;
            command.text = std::string(picture.control);
            command.intValue = (*value - 50) * 2;
            return submit(
                std::move(command), "RenderingControl", action);
        }
    }

    if (action == "ListPresets") {
        return soapResponse(
            "RenderingControl", action,
            "<CurrentPresetNameList>FactoryDefaults"
            "</CurrentPresetNameList>");
    }
    if (action == "SelectPreset") {
        const auto preset = body.value("PresetName");
        if (!preset)
            return soapFault(kInvalidArgs, "Invalid Args");
        if (lower(*preset) != "factorydefaults") {
            return soapFault(kInvalidName, "Invalid Name");
        }
        playback::PlaybackCommand command;
        command.type =
            playback::PlaybackCommandType::ResetPictureControls;
        return submit(std::move(command), "RenderingControl", action);
    }
    return soapFault(kInvalidAction, "Invalid Action");
}

HttpResponse SoapRouter::handleConnectionManager(
    const std::string &action, const SoapRequestXml &body)
{
    const playback::PlaybackSnapshot snapshot =
        m_session.snapshot().playback;
    const bool hasConnection =
        snapshot.state != playback::PlaybackState::NoMedia;
    if (action == "GetProtocolInfo") {
        return soapResponse(
            "ConnectionManager", action,
            "<Source></Source><Sink>"
                + UpnpXml::sinkProtocolInfo() + "</Sink>");
    }
    if (action == "GetCurrentConnectionIDs") {
        return soapResponse(
            "ConnectionManager", action,
            "<ConnectionIDs>" + std::string(hasConnection ? "0" : "")
                + "</ConnectionIDs>");
    }
    if (action == "GetCurrentConnectionInfo") {
        const auto connectionId = body.value("ConnectionID");
        if (!connectionId)
            return soapFault(kInvalidArgs, "Invalid Args");
        const auto id = parseInt(*connectionId);
        if (!id || *id != 0 || !hasConnection)
            return soapFault(
                kInvalidConnectionReference,
                "Invalid connection reference");
        return soapResponse(
            "ConnectionManager", action,
            "<RcsID>-1</RcsID><AVTransportID>0</AVTransportID>"
            "<ProtocolInfo>http-get:*:*:*</ProtocolInfo>"
            "<PeerConnectionManager></PeerConnectionManager>"
            "<PeerConnectionID>-1</PeerConnectionID>"
            "<Direction>Input</Direction><Status>OK</Status>");
    }
    return soapFault(kInvalidAction, "Invalid Action");
}

HttpResponse SoapRouter::submit(
    playback::PlaybackCommand command, const std::string &service,
    const std::string &action)
{
    const auto submission = m_session.submit(std::move(command));
    if (submission.queued)
        return soapResponse(service, action);
    if (submission.status == playback::PlaybackSubmissionStatus::SessionEnding)
        return soapFault(
            kTransitionNotAvailable, "Transition not available");
    if (submission.status
        == playback::PlaybackSubmissionStatus::InvalidCommand) {
        return soapFault(kInvalidArgs, "Invalid Args");
    }
    return soapFault(kActionFailed, "Action Failed");
}

HttpResponse SoapRouter::submitMedia(
    playback::MediaRequest media, const std::string &service,
    const std::string &action)
{
    const auto submission =
        m_session.openMedia(media, m_options.now());
    if (submission.queued)
        return soapResponse(service, action);
    if (submission.status == playback::PlaybackSubmissionStatus::SessionEnding)
        return soapFault(
            kTransitionNotAvailable, "Transition not available");
    if (submission.status
        == playback::PlaybackSubmissionStatus::InvalidCommand)
        return soapFault(kInvalidArgs, "Invalid Args");
    return soapFault(kActionFailed, "Action Failed");
}

void SoapRouter::log(const std::string &message) const
{
    if (m_options.logger)
        m_options.logger(message);
}

} // namespace mcast::dlna::soap
