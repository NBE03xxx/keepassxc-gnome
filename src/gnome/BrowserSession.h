// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <map>
#include <memory>

class CoreSession;
class CheckpointStore;

// Only the trusted worker controller assigns connection IDs and decides approvals.
// Each instance is tied to an unlocked model and its recovery store.
class BrowserSession
{
public:
    static constexpr qsizetype MaxFrame = 64 * 1024;

    BrowserSession(CoreSession& database, CheckpointStore& checkpoints);
    ~BrowserSession();
    QJsonObject request(int connection, const QJsonObject& message);
    QJsonObject decide(int connection, const QString& token, const QString& selection);
    void close(int connection);
    void modelChanged() { ++revision; }

private:
    struct Peer;
    CoreSession& database;
    CheckpointStore& checkpoints;
    std::map<int, std::unique_ptr<Peer>> peers;
    quint64 revision = 0;
};
