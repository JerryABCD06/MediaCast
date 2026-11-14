// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/MediaTypes.h"

#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace mcast::avalonia::qtfree {

struct MetadataPipelineResult
{
    std::string title;
    std::string subtitle;
    std::string artist;
    std::string album;
    std::string artworkSource;
    std::vector<std::string> rejected;
};

class MetadataPipeline
{
public:
    MetadataPipeline();

    bool loadFile(
        const std::string &path,
        std::string *error = nullptr);
    bool loadNearExecutable(std::string *error = nullptr);
    bool loadedFromFile() const noexcept { return m_loadedFromFile; }
    const std::string &sourcePath() const noexcept { return m_sourcePath; }

    MetadataPipelineResult resolve(
        const MediaRequest &request,
        const std::map<
            std::string, std::string, std::less<>> &fileTags,
        std::string_view fileArtworkSource) const;

private:
    struct FieldRule
    {
        std::vector<std::string> sources;
        std::vector<std::regex> rejectPatterns;
    };

    void useBuiltInRules();
    bool applyConfig(std::string_view json, std::string *error);
    std::string resolveSource(
        const std::string &source,
        const MediaRequest &request,
        const std::map<
            std::string, std::string, std::less<>> &fileTags,
        std::string_view fileArtworkSource) const;
    std::string acceptCandidate(
        const std::string &candidate, const std::string &field,
        const FieldRule &rule, bool titleRules,
        std::vector<std::string> &rejected) const;
    bool placeholder(std::string_view value) const;
    bool validTitle(std::string_view value) const;

    FieldRule m_title;
    FieldRule m_artist;
    FieldRule m_album;
    FieldRule m_subtitleAudio;
    FieldRule m_subtitleVisual;
    std::vector<std::string> m_artworkSources;
    std::vector<std::string> m_placeholders;
    std::vector<std::string> m_mediaExtensions;
    int m_titleMinimumScalars = 2;
    int m_titleMinimumLetters = 1;
    int m_titleMaximumDigitRun = 4;
    int m_titleMaximumDigitRatioPercent = 40;
    bool m_loadedFromFile = false;
    std::string m_sourcePath;
};

} // namespace mcast::avalonia::qtfree

