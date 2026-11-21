// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <functional>
#include <memory>

class BrowserIntegration
{
public:
    enum class State { Disabled, Enabled, Conflict, Unavailable };
    using Request = std::function<void(int connection, const QJsonObject& message)>;
    using Disconnect = std::function<void(int connection)>;

    // Calls and callbacks run on the GTK main context.
    BrowserIntegration(Request request, Disconnect disconnect);
    ~BrowserIntegration();
    State state() const;
    State enable();
    State disable();
    bool connected(int connection) const;
    void send(int connection, const QJsonObject& response);
    void closeConnections();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
