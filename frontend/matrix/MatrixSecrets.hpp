// OBS Matrix Studio: user-scoped Windows DPAPI protection for stream keys and API token.
#pragma once

#include <QByteArray>
#include <QString>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace MatrixSecrets {

inline QString protect(const QString &plain)
{
    if (plain.isEmpty())
        return {};
#ifdef _WIN32
    const QByteArray bytes = plain.toUtf8();
    DATA_BLOB input{DWORD(bytes.size()), reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))};
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&input, L"OBS Matrix Studio", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted))
        return {};
    const QByteArray result(reinterpret_cast<const char *>(encrypted.pbData), int(encrypted.cbData));
    LocalFree(encrypted.pbData);
    return QStringLiteral("dpapi:") + QString::fromLatin1(result.toBase64());
#else
    return {};
#endif
}

inline QString unprotect(const QString &stored)
{
    if (stored.isEmpty())
        return {};
    if (!stored.startsWith("dpapi:"))
        return stored; // Migrates old plaintext values on next save.
#ifdef _WIN32
    const QByteArray encryptedBytes = QByteArray::fromBase64(stored.mid(6).toLatin1());
    DATA_BLOB input{DWORD(encryptedBytes.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(encryptedBytes.data()))};
    DATA_BLOB decrypted{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &decrypted))
        return {};
    const QByteArray result(reinterpret_cast<const char *>(decrypted.pbData), int(decrypted.cbData));
    LocalFree(decrypted.pbData);
    return QString::fromUtf8(result);
#else
    return {};
#endif
}

} // namespace MatrixSecrets
