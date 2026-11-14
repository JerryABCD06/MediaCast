// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/MediaTypes.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace mcast::avalonia::qtfree;

    check(
        titleFromUri(
            "http://host/Cold-Enough_For_Snow.mp4?token=1")
            == "Cold Enough For Snow",
        "URI title fallback");
    check(
        titleFromUri(
            "http://host/%E5%8E%9F%E7%A5%9E-%E5%AE%A3%E4%BC%A0%E7%89%87.mp4")
            == "原神 宣传片",
        "UTF-8 percent-decoded title");
    check(mediaKindFromUri("photo.webp") == MediaKind::Image,
          "image media kind");
    check(mediaKindFromUri("song.flac?x=1") == MediaKind::Audio,
          "audio media kind");
    check(mediaKindFromUri("movie") == MediaKind::Video,
          "unknown extension defaults to video");
    check(looksLikePlaceholder("\" UNKOWN \""),
          "placeholder is case-insensitive");
    check(looksLikeATitle("A Real Movie"), "real title accepted");
    check(!looksLikeATitle("417421081321234"),
          "numeric identifier rejected");
    check(!looksLikeATitle("原"),
          "title length counts UTF-8 scalars");
    check(looksLikeATitle("视频"),
          "two Unicode scalars form a title");
    check(simplified("原神 😀") == "原神 😀",
          "Unicode text preserved");
    check(
        sameMediaUri(
            "HTTP://Example.COM:80/%7euser/a%2fb",
            "http://example.com/~user/a%2Fb"),
        "URI comparison normalizes scheme, host, port and escaping");
    check(
        !sameMediaUri(
            "https://example.com:8443/a",
            "https://example.com/a"),
        "non-default port is significant");
    check(
        resolveUriReference(
            "https://example.com/media/movie.mp4",
            "cover art.png")
            == "https://example.com/media/cover art.png",
        "relative artwork resolves against media");
    check(
        fileUriFromPath("C:\\Media Files\\封面.png")
            == "file:///C:/Media%20Files/%E5%B0%81%E9%9D%A2.png",
        "local artwork path becomes UTF-8 file URI");
    check(isHttpArtworkUri("HTTPS://example.com/cover.png"),
          "HTTP artwork accepted");
    check(!isHttpArtworkUri("file:///C:/cover.png"),
          "local file is not a remote artwork URI");

    std::cout << "Qt-free media type tests passed\n";
    return 0;
}
