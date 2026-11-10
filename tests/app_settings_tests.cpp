// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

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

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    const QString path = directory.filePath(QStringLiteral("MediaCast.json"));

    const QByteArray invalid = QByteArrayLiteral(R"({"ui": broken})");
    {
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "write invalid settings");
        file.write(invalid);
    }

    {
        AppSettings settings(path);
        check(!settings.isPersistent(), "invalid settings are not persistent");
        settings.setLanguage(QStringLiteral("en_US"));
    }
    check(readAll(path) == invalid,
          "invalid settings survive later setting changes");

    std::cout << "AppSettings corruption tests passed\n";
    return 0;
}
