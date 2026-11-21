// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <algorithm>

inline bool validBrowserConnectionName(const QString& name)
{
    return !name.isEmpty() && name.size() <= 64 && name == name.trimmed()
        && name == name.normalized(QString::NormalizationForm_C)
        && std::all_of(name.cbegin(), name.cend(), [](QChar ch) {
            return ch.isLetterOrNumber() || ch.isMark() || ch == u' ' || ch == u'-' || ch == u'_';
        });
}
