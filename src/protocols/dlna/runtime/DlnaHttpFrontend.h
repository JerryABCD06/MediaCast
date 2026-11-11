// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"
#include "protocols/dlna/net/HttpServer.h"
#include "protocols/dlna/runtime/GenaManager.h"

#include <string>

namespace mcast::dlna::runtime {

struct DlnaHttpFrontendOptions {
    std::string friendlyName;
    std::string udn;
    GenaManager *gena = nullptr;
};

// DLNA HTTP 面的纯路由层。GET 只暴露设备描述和声明过的 SCPD；
// POST 统一交给 Claim handler，确保 SOAP 校验与异步接管只有一份实现。
class DlnaHttpFrontend
{
public:
    DlnaHttpFrontend(
        claim::DlnaClaimHttpHandler &soap,
        DlnaHttpFrontendOptions options);

    net::HttpDispatchResult handle(
        const mcast::dlna::core::HttpRequest &request);

private:
    claim::DlnaClaimHttpHandler &m_soap;
    DlnaHttpFrontendOptions m_options;
};

} // namespace mcast::dlna::runtime
