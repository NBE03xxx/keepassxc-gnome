// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <iostream>

#include <QJsonDocument>
#include <QJsonObject>

#include "browser/BrowserMessageBuilder.h"

namespace
{
constexpr std::uint32_t MaxFrame = 64 * 1024;
constexpr auto Hash = "97ac79e04c187bc8530cacfc57afa166e18a77806b511bf8b5a0f2746ce287f05";

class Connection
{
public:
    QJsonObject respond(const QJsonObject& request)
    {
        const auto action = request.value("action").toString();
        const auto nonce = request.value("nonce").toString();
        if (action != "change-public-keys" && action != "get-databasehash") {
            return m_builder.getErrorReply(action, ERROR_KEEPASS_INCORRECT_ACTION);
        }
        if (m_builder.base64Decode(nonce).size() != crypto_box_NONCEBYTES) {
            return m_builder.getErrorReply(action, ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE);
        }
        const auto responseNonce = m_builder.incrementNonce(nonce);
        if (action == "change-public-keys") {
            const auto clientKey = request.value("publicKey").toString();
            if (m_builder.base64Decode(clientKey).size() != crypto_box_PUBLICKEYBYTES) {
                return m_builder.getErrorReply(action, ERROR_KEEPASS_CLIENT_PUBLIC_KEY_NOT_RECEIVED);
            }
            const auto [publicKey, secretKey] = m_builder.getKeyPair();
            m_clientKey = clientKey;
            m_secretKey = secretKey;
            auto response = m_builder.buildMessage(responseNonce);
            response["action"] = action;
            response["publicKey"] = publicKey;
            return response;
        }
        if (m_secretKey.isEmpty()) {
            return m_builder.getErrorReply(action, ERROR_KEEPASS_CLIENT_PUBLIC_KEY_NOT_RECEIVED);
        }
        const auto payload = m_builder.decryptMessage(
            request.value("message").toString(), nonce, m_clientKey, m_secretKey, MaxFrame);
        if (payload.value("action").toString() != action) {
            return m_builder.getErrorReply(action, ERROR_KEEPASS_CANNOT_DECRYPT_MESSAGE);
        }
        return m_builder.buildResponse(action, responseNonce, {{"hash", Hash}}, m_clientKey, m_secretKey);
    }

private:
    BrowserMessageBuilder m_builder;
    QString m_clientKey;
    QString m_secretKey;
};
} // namespace

int main()
{
    Connection connection;
    std::uint32_t length = 0;
    while (std::cin.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        if (length == 0 || length > MaxFrame) {
            return 1;
        }
        QByteArray frame(length, Qt::Uninitialized);
        if (!std::cin.read(frame.data(), length)) {
            return 1;
        }
        if (!frame.isValidUtf8()) {
            return 1;
        }
        const auto request = QJsonDocument::fromJson(frame);
        if (!request.isObject()) {
            return 1;
        }
        const auto response = QJsonDocument(connection.respond(request.object())).toJson(QJsonDocument::Compact);
        if (response.size() > MaxFrame) {
            return 1;
        }
        length = response.size();
        std::cout.write(reinterpret_cast<const char*>(&length), sizeof(length));
        std::cout.write(response.constData(), length).flush();
        if (!std::cout) {
            return 1;
        }
    }
    return std::cin.eof() && std::cin.gcount() == 0 ? 0 : 1;
}
