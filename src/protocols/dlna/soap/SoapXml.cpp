// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/soap/SoapXml.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>

namespace mcast::dlna::soap {
namespace {

constexpr std::size_t kMaxXmlDepth = 24;
constexpr std::size_t kMaxXmlNodes = 512;
constexpr std::wstring_view kSoapEnvelopeNamespace =
    L"http://schemas.xmlsoap.org/soap/envelope/";

class ComApartment
{
public:
    ComApartment()
    {
        const HRESULT result =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_usable = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
        m_uninitialize = result == S_OK || result == S_FALSE;
    }

    ~ComApartment()
    {
        if (m_uninitialize)
            CoUninitialize();
    }

    bool usable() const noexcept { return m_usable; }

private:
    bool m_usable = false;
    bool m_uninitialize = false;
};

class XmlReader
{
public:
    XmlReader()
    {
        m_usable =
            SUCCEEDED(CreateXmlReader(
                __uuidof(IXmlReader),
                reinterpret_cast<void **>(&m_reader), nullptr))
            && m_reader;
    }

    ~XmlReader();

    bool open(std::string_view xml, std::string &error)
    {
        if (!m_usable) {
            error = "XmlLite reader creation failed";
            return false;
        }
        if (xml.size() > static_cast<std::size_t>(
                std::numeric_limits<DWORD>::max())) {
            error = "XML document is too large for XmlLite";
            return false;
        }
        m_stream = SHCreateMemStream(
            reinterpret_cast<const BYTE *>(xml.data()),
            static_cast<UINT>(xml.size()));
        if (!m_stream) {
            error = "XML memory stream creation failed";
            return false;
        }
        if (FAILED(CreateXmlReaderInputWithEncodingName(
                m_stream, nullptr, L"utf-8", FALSE, nullptr,
                &m_input))) {
            error = "XmlLite input creation failed";
            return false;
        }
        if (FAILED(m_reader->SetProperty(
                XmlReaderProperty_DtdProcessing,
                DtdProcessing_Prohibit))) {
            error = "XmlLite DTD prohibition failed";
            return false;
        }
        if (FAILED(m_reader->SetInput(m_input))) {
            error = "XmlLite SetInput failed";
            return false;
        }
        return true;
    }

    HRESULT read()
    {
        XmlNodeType type = XmlNodeType_None;
        return m_reader->Read(&type);
    }
    IXmlReader *get() const noexcept { return m_reader; }

private:
    IXmlReader *m_reader = nullptr;
    IStream *m_stream = nullptr;
    IXmlReaderInput *m_input = nullptr;
    bool m_usable = false;

public:
    XmlReader(const XmlReader &) = delete;
    XmlReader &operator=(const XmlReader &) = delete;
};

std::optional<std::string> utf8(const wchar_t *value, UINT length)
{
    if (!value)
        return std::nullopt;
    if (length == 0)
        return std::string{};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, static_cast<int>(length),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return std::nullopt;
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, static_cast<int>(length),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<std::string> localName(IXmlReader *reader)
{
    const wchar_t *value = nullptr;
    UINT length = 0;
    if (FAILED(reader->GetLocalName(&value, &length)))
        return std::nullopt;
    return utf8(value, length);
}

std::optional<std::string> namespaceUri(IXmlReader *reader)
{
    const wchar_t *value = nullptr;
    UINT length = 0;
    if (FAILED(reader->GetNamespaceUri(&value, &length)))
        return std::nullopt;
    return utf8(value, length);
}

std::optional<std::string> nodeValue(IXmlReader *reader)
{
    const wchar_t *value = nullptr;
    UINT length = 0;
    if (FAILED(reader->GetValue(&value, &length)))
        return std::nullopt;
    return utf8(value, length);
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

bool validName(std::string_view name)
{
    return !name.empty()
        && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_' || ch == '-';
        });
}

} // namespace

XmlReader::~XmlReader()
{
    if (m_reader)
        m_reader->Release();
    if (m_input)
        m_input->Release();
    if (m_stream)
        m_stream->Release();
}

std::optional<std::string> SoapRequestXml::value(
    std::string_view name) const
{
    for (const auto &[key, value] : parameters) {
        if (key == name)
            return value;
    }
    return std::nullopt;
}

bool parseSoapRequest(
    std::string_view xml, std::string_view expectedService,
    SoapRequestXml &request, std::string &error)
{
    request = {};
    error.clear();
    ComApartment com;
    if (!com.usable()) {
        error = "COM initialization failed";
        return false;
    }
    XmlReader reader;
    if (!reader.open(xml, error))
        return false;

    const std::string expectedNamespace =
        "urn:schemas-upnp-org:service:" + std::string(expectedService)
        + ":1";
    bool envelopeSeen = false;
    bool bodySeen = false;
    bool bodyOpen = false;
    bool actionSeen = false;
    bool actionClosed = false;
    std::size_t envelopeDepth = 0;
    std::size_t actionDepth = 0;
    std::size_t parameterDepth = 0;
    bool parameterActive = false;
    std::string parameterName;
    std::string parameterText;
    std::map<std::string, std::string> parameters;
    std::size_t nodes = 0;

    HRESULT result = reader.read();
    while (result == S_OK) {
        if (++nodes > kMaxXmlNodes) {
            error = "SOAP XML has too many nodes";
            return false;
        }
        UINT depth = 0;
        if (FAILED(reader.get()->GetDepth(&depth)))
            return false;
        if (depth >= kMaxXmlDepth) {
            error = "SOAP XML nesting is too deep";
            return false;
        }
        XmlNodeType type = XmlNodeType_None;
        reader.get()->GetNodeType(&type);

        if (type == XmlNodeType_Element) {
            const auto name = localName(reader.get());
            const auto ns = namespaceUri(reader.get());
            if (!name || !ns) {
                error = "SOAP XML has an invalid element name";
                return false;
            }
            if (depth == 0) {
                if (envelopeSeen || *name != "Envelope"
                    || *ns != utf8(kSoapEnvelopeNamespace.data(),
                                   static_cast<UINT>(
                                       kSoapEnvelopeNamespace.size()))
                        .value_or("")) {
                    error = "SOAP Envelope is invalid";
                    return false;
                }
                envelopeSeen = true;
                envelopeDepth = 0;
            } else if (depth == 1 && *name == "Body"
                       && *ns
                           == utf8(
                                  kSoapEnvelopeNamespace.data(),
                                  static_cast<UINT>(
                                      kSoapEnvelopeNamespace.size()))
                                  .value_or("")) {
                if (bodySeen) {
                    error = "SOAP Body appears more than once";
                    return false;
                }
                bodySeen = true;
                bodyOpen = true;
            } else if (bodyOpen && depth == 2) {
                if (actionSeen || *ns != expectedNamespace
                    || !validName(*name)) {
                    error = "SOAP Body action is invalid";
                    return false;
                }
                actionSeen = true;
                actionDepth = depth;
                request.action = *name;
            } else if (actionSeen && !actionClosed
                       && depth == actionDepth + 1) {
                if (parameterActive || !validName(*name)) {
                    error = "SOAP action parameter is invalid: depth="
                        + std::to_string(depth) + " name=" + *name
                        + " ns=" + *ns;
                    return false;
                }
                parameterActive = true;
                parameterDepth = depth;
                parameterName = *name;
                parameterText.clear();
            } else if (parameterActive && depth > parameterDepth) {
                error = "SOAP parameter contains nested elements";
                return false;
            }
        } else if (
            type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
            if (parameterActive) {
                const auto value = nodeValue(reader.get());
                if (!value) {
                    error = "SOAP text node is invalid";
                    return false;
                }
                parameterText += *value;
            }
        } else if (type == XmlNodeType_EndElement) {
            const auto name = localName(reader.get());
            const auto ns = namespaceUri(reader.get());
            if (!name)
                return false;
            if (*name == "Body" && bodyOpen && ns
                && *ns
                    == utf8(
                           kSoapEnvelopeNamespace.data(),
                           static_cast<UINT>(
                               kSoapEnvelopeNamespace.size()))
                           .value_or("")) {
                bodyOpen = false;
            } else if (parameterActive && *name == parameterName) {
                if (parameters.contains(parameterName)) {
                    error = "SOAP parameter appears more than once";
                    return false;
                }
                parameters.emplace(
                    parameterName, trim(parameterText));
                parameterActive = false;
            } else if (actionSeen && *name == request.action) {
                actionClosed = true;
            }
        }
        result = reader.read();
    }
    if (result != S_FALSE) {
        error = "SOAP XML is malformed";
        return false;
    }
    if (!envelopeSeen || !bodySeen || !actionSeen || !actionClosed) {
        error = "SOAP Envelope, Body or action is missing";
        return false;
    }
    request.parameters.assign(parameters.begin(), parameters.end());
    return true;
}

bool parseDidlMetadata(
    std::string_view xml, DidlMetadata &metadata, std::string &error)
{
    metadata = {};
    error.clear();
    if (xml.empty())
        return true;
    ComApartment com;
    if (!com.usable()) {
        error = "COM initialization failed";
        return false;
    }
    XmlReader reader;
    if (!reader.open(xml, error))
        return false;

    struct Field {
        std::size_t depth = 0;
        std::string name;
        std::optional<std::string> *target = nullptr;
        std::string text;
    };
    std::optional<Field> active;
    std::map<std::string, std::optional<std::string> *> fields{
        {"title", &metadata.title},
        {"class", &metadata.upnpClass},
        {"artist", &metadata.artist},
        {"creator", &metadata.creator},
        {"album", &metadata.album},
        {"subtitle", &metadata.subtitle},
        {"description", &metadata.description},
        {"longDescription", &metadata.longDescription},
    };
    std::size_t nodes = 0;
    HRESULT result = reader.read();
    while (result == S_OK) {
        if (++nodes > kMaxXmlNodes) {
            error = "DIDL metadata has too many nodes";
            return false;
        }
        UINT depth = 0;
        reader.get()->GetDepth(&depth);
        if (depth >= kMaxXmlDepth) {
            error = "DIDL metadata nesting is too deep";
            return false;
        }
        XmlNodeType type = XmlNodeType_None;
        reader.get()->GetNodeType(&type);
        if (type == XmlNodeType_Element) {
            const auto name = localName(reader.get());
            if (!name)
                return false;
            if (!active) {
                const auto found = fields.find(*name);
                if (found != fields.end() && !*found->second) {
                    active = Field{depth, found->first, found->second, {}};
                }
            }
        } else if (
            type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
            if (active) {
                const auto value = nodeValue(reader.get());
                if (!value)
                    return false;
                active->text += *value;
            }
        } else if (type == XmlNodeType_EndElement) {
            const auto name = localName(reader.get());
            if (active && name && *name == active->name) {
                *active->target = trim(active->text);
                active.reset();
            }
        }
        result = reader.read();
    }
    if (result != S_FALSE) {
        error = "DIDL metadata is malformed";
        return false;
    }
    return true;
}

} // namespace mcast::dlna::soap
