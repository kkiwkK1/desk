#include "widget/secrets.h"

#include <windows.h>
#include <wincred.h>

namespace wg {

bool SaveSecret(const std::wstring& target, const std::wstring& user,
                const std::wstring& secret) {
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.UserName = const_cast<LPWSTR>(user.c_str());
    cred.CredentialBlobSize = (DWORD)(secret.size() * sizeof(wchar_t));
    cred.CredentialBlob = (LPBYTE)secret.c_str();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) != FALSE;
}

std::wstring LoadSecret(const std::wstring& target) {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) return L"";
    std::wstring out((const wchar_t*)cred->CredentialBlob,
                     cred->CredentialBlobSize / sizeof(wchar_t));
    CredFree(cred);
    return out;
}

bool DeleteSecret(const std::wstring& target) {
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
}

} // namespace wg
