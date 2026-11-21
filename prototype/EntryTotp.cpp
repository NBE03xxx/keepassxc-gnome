#include "CoreSession.h"

#include "core/Base32.h"
#include "core/Clock.h"
#include "core/Entry.h"
#include "core/Totp.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>
#include <limits>

namespace
{
using Settings = QSharedPointer<Totp::Settings>;

bool hasTotpAttributes(const Entry& entry)
{
    const auto* attrs = entry.attributes();
    return attrs->contains(Totp::ATTRIBUTE_OTP) || attrs->contains(Totp::ATTRIBUTE_SEED)
        || attrs->contains(Totp::ATTRIBUTE_SETTINGS) || attrs->contains(Totp::KP2_TOTP_SECRET)
        || attrs->contains(Totp::KP2_TOTP_ALGORITHM) || attrs->contains(Totp::KP2_TOTP_LENGTH)
        || attrs->contains(Totp::KP2_TOTP_PERIOD);
}

void clearAlternateTotpAttributes(Entry& entry)
{
    for (const auto& name : {Totp::ATTRIBUTE_SEED, Totp::ATTRIBUTE_SETTINGS,
                             Totp::KP2_TOTP_SECRET, Totp::KP2_TOTP_ALGORITHM,
                             Totp::KP2_TOTP_LENGTH, Totp::KP2_TOTP_PERIOD}) {
        entry.attributes()->remove(name);
    }
}

bool canGenerate(const Settings& settings)
{
    if (!settings || settings->key.isEmpty() || settings->key.size() > TotpCode::MaximumInputBytes
        || settings->step == 0 || settings->step > TotpCode::MaximumPeriodSeconds
        || settings->digits == 0 || settings->digits > TotpCode::MaximumDigits) return false;
    // The pinned upstream generator stores alphabet^digits in a quint32.
    const auto alphabetSize = settings->encoder.alphabet.size();
    if (alphabetSize < 2) return false;
    quint64 values = 1;
    for (unsigned i = 0; i < settings->digits; ++i) {
        values *= alphabetSize;
        if (values > std::numeric_limits<quint32>::max()) return false;
    }
    // Check the decoded bytes directly. A QVariant containing an empty QByteArray is not null in Qt 6.
    return !Base32::decode(Base32::sanitizeInput(settings->key.toLatin1())).isEmpty();
}

QString normalizedSeed(QString seed)
{
    static const QRegularExpression whitespace(QStringLiteral("[ \\t\\r\\n]"));
    static const QRegularExpression base32(QStringLiteral("\\A[A-Za-z2-7]+={0,6}\\z"));
    seed.remove(whitespace);
    if (!base32.match(seed).hasMatch()) return {};
    seed = seed.toUpper();
    auto encoded = seed.toLatin1();
    const auto padded = Base32::sanitizeInput(encoded);
    if (encoded.contains('=') && encoded != padded) return {};
    return QString::fromLatin1(padded);
}

bool validNumber(const QString& number, uint maximum)
{
    static const QRegularExpression positiveInteger(QStringLiteral("\\A[0-9]+\\z"));
    bool ok = false;
    const auto result = number.toUInt(&ok);
    return ok && result > 0 && result <= maximum && positiveInteger.match(number).hasMatch();
}

bool parseUri(const QString& input, QString* canonical = nullptr)
{
    QUrl uri(input, QUrl::StrictMode);
    if (!uri.isValid() || uri.scheme() != "otpauth" || uri.host() != "totp" || uri.hasFragment()
        || !uri.userInfo().isEmpty() || uri.port() != -1) return false;
    QUrlQuery query(uri);
    QSet<QString> keys;
    QString seed;
    for (const auto& [name, value] : query.queryItems(QUrl::FullyDecoded)) {
        if (keys.contains(name)) return false;
        keys.insert(name);
        if (name == "secret") {
            seed = canonical ? normalizedSeed(value) : value;
        } else if (name == "digits") {
            if (!validNumber(value, TotpCode::MaximumDigits)) return false;
        } else if (name == "period") {
            if (!validNumber(value, TotpCode::MaximumPeriodSeconds)) return false;
        } else if (name == "algorithm") {
            auto algorithm = value.toUpper();
            if (algorithm != "SHA1" && algorithm != "SHA256" && algorithm != "SHA512") return false;
        } else if (name == "encoder") {
            if (value != "steam") return false;
        } else if (canonical && name != "issuer") {
            return false;
        }
    }
    if (seed.isEmpty()) return false;
    if (canonical) {
        query.removeQueryItem(QStringLiteral("secret"));
        query.addQueryItem(QStringLiteral("secret"), seed);
        uri.setQuery(query);
        *canonical = uri.toString(QUrl::FullyEncoded);
    }
    return true;
}
}

TotpCode CoreSession::totpCode(const std::string& id) const
{
    using Status = TotpCode::Status;
    const auto* entry = findEntry(id);
    if (!entry || entry->isRecycled()) return {};
    const auto settings = entry->totpSettings();
    if (!settings) return {hasTotpAttributes(*entry) ? Status::Invalid : Status::NotConfigured};
    if (!canGenerate(settings)) return {Status::Invalid};
    const auto* attrs = entry->attributes();
    if (settings->format == Totp::OTPURL && attrs->contains(Totp::ATTRIBUTE_OTP)
        && !parseUri(attrs->value(Totp::ATTRIBUTE_OTP))) return {Status::Invalid};
    const auto now = Clock::currentMilliSecondsSinceEpoch();
    if (now < 1000) return {Status::Invalid};
    const auto period = std::int64_t(settings->step) * 1000;
    const auto start = now - now % period;
    bool valid = false;
    auto code = Totp::generateTotp(settings, &valid, quint64(now / 1000));
    if (!valid) return {Status::Invalid};
    auto value = code.toStdString();
    ::explicit_bzero(code.data(), code.size() * sizeof(QChar));
    return {Status::Ready, std::move(value), start, start + period};
}

bool CoreSession::setTotp(const std::string& id, const std::string& input)
{
    auto* entry = findEntry(id);
    if (!entry || entry->isRecycled() || input.empty() || input.size() > TotpCode::MaximumInputBytes) return false;
    auto value = QString::fromStdString(input).trimmed();
    QString canonical;
    Settings settings;
    if (value.startsWith(QStringLiteral("otpauth:"), Qt::CaseInsensitive)) {
        if (parseUri(value, &canonical)) settings = Totp::parseSettings(canonical);
    } else {
        settings = Totp::createSettings(normalizedSeed(value));
    }
    if (!canGenerate(settings)) return false;

    // Serialize bare keys as KeePassXC does and keep the issuer and label of imported URIs.
    if (canonical.isEmpty()) {
        canonical = Totp::writeSettings(settings, entry->resolveMultiplePlaceholders(entry->title()),
                                       entry->resolveMultiplePlaceholders(entry->username()));
    }
    // Entry::setTotp starts a history operation itself and does not clear TimeOtp-* fields.
    entry->beginUpdate();
    clearAlternateTotpAttributes(*entry);
    entry->attributes()->set(Totp::ATTRIBUTE_OTP, canonical, true);
    entry->endUpdate();
    return true;
}

bool CoreSession::removeTotp(const std::string& id)
{
    auto* entry = findEntry(id);
    if (!entry || entry->isRecycled() || !hasTotpAttributes(*entry)) return false;
    entry->beginUpdate();
    clearAlternateTotpAttributes(*entry);
    entry->attributes()->remove(Totp::ATTRIBUTE_OTP);
    entry->endUpdate();
    return true;
}
