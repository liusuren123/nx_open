// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

/*
 * Removes stale Burn registrations ("Apps & Features" entries) of this product that were left
 * behind by previously installed builds.
 *
 * WiX 3.14 generates a random registration id for every bundle it links and Burn does not remove
 * related bundles of the same version, so every rebuild followed by a reinstall adds another
 * "Nx Meta Client" entry. Besides the clutter, a stale entry's uninstall removes the shared client
 * MSI, i.e. the application, its shortcuts and the client settings.
 *
 * The installer runs this helper on every install (see the ExePackage in client_package.wxs), so
 * only the registration of the bundle that is being installed survives.
 */

#include <windows.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kUninstallSubKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
constexpr const wchar_t* kDependenciesSubKey = L"SOFTWARE\\Classes\\Installer\\Dependencies";

struct StaleBundle
{
    std::wstring keyName;
    std::wstring providerKey;
    bool hasCache{};
    REGSAM view{};
};

std::wstring toLower(std::wstring value)
{
    for (auto& c: value)
        c = static_cast<wchar_t>(towlower(c));

    return value;
}

std::wstring readValue(HKEY key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};

    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return {};

    std::wstring result(size / sizeof(wchar_t), L'\0');
    const auto status = RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(result.data()), &size);
    if (status != ERROR_SUCCESS)
        return {};

    while (!result.empty() && result.back() == L'\0')
        result.pop_back();

    return result;
}

void collectStaleBundles(const std::wstring& displayName, const std::wstring& keepProviderKey,
    REGSAM view, std::vector<StaleBundle>& result)
{
    HKEY uninstall = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallSubKey, 0, KEY_READ | view, &uninstall)
        != ERROR_SUCCESS)
    {
        return;
    }

    for (DWORD index = 0; ; ++index)
    {
        wchar_t name[256];
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(uninstall, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr)
            == ERROR_NO_MORE_ITEMS)
        {
            break;
        }

        HKEY entry = nullptr;
        if (RegOpenKeyExW(uninstall, name, 0, KEY_READ | view, &entry) != ERROR_SUCCESS)
            continue;

        const auto entryDisplayName = toLower(readValue(entry, L"DisplayName"));
        const auto providerKey = toLower(readValue(entry, L"BundleProviderKey"));
        const auto hasCache = !readValue(entry, L"BundleCachePath").empty();
        RegCloseKey(entry);

        // Only Burn registrations of this product are of interest.
        if (providerKey.empty() || entryDisplayName != displayName)
            continue;

        // The registration of the bundle that is currently being installed.
        if (!keepProviderKey.empty() && providerKey == keepProviderKey)
            continue;

        result.push_back({name, providerKey, hasCache, view});
    }

    RegCloseKey(uninstall);
}

void deleteDirectoryRecursively(const std::wstring& path)
{
    const auto pattern = path + L"\\*";
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
                continue;

            const auto child = path + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                deleteDirectoryRecursively(child);
            }
            else
            {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &data));

        FindClose(find);
    }

    RemoveDirectoryW(path.c_str());
}

void removeStaleBundle(const StaleBundle& bundle)
{
    HKEY uninstall = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallSubKey, 0, KEY_READ | KEY_WRITE | bundle.view,
        &uninstall) == ERROR_SUCCESS)
    {
        RegDeleteTreeW(uninstall, bundle.keyName.c_str());
        RegCloseKey(uninstall);
    }

    // The dependency provider registration created for the bundle.
    for (const auto view: {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
    {
        HKEY dependencies = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDependenciesSubKey, 0, KEY_READ | KEY_WRITE | view,
            &dependencies) == ERROR_SUCCESS)
        {
            RegDeleteTreeW(dependencies, bundle.providerKey.c_str());
            RegCloseKey(dependencies);
        }
    }

    if (!bundle.hasCache)
        return;

    wchar_t programData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", programData, std::size(programData)) == 0)
        return;

    // The folder is named after the registration key.
    std::wstring cacheDirectory = programData;
    cacheDirectory += L"\\Package Cache\\";
    cacheDirectory += bundle.keyName;
    deleteDirectoryRecursively(cacheDirectory);
}

} // namespace

namespace {

void log(const wchar_t* format, ...)
{
    wchar_t buffer[512];

    va_list arguments;
    va_start(arguments, format);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, format, arguments);
    va_end(arguments);

    OutputDebugStringW(buffer);
    OutputDebugStringW(L"\n");
}

} // namespace

int main()
{
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return 0;

    std::wstring displayName;
    std::wstring keepProviderKey;
    bool dryRun = false;

    for (int i = 1; i < argumentCount; ++i)
    {
        const std::wstring argument = arguments[i];
        if (argument == L"--display-name" && i + 1 < argumentCount)
            displayName = arguments[++i];
        else if (argument == L"--keep-provider-key" && i + 1 < argumentCount)
            keepProviderKey = arguments[++i];
        else if (argument == L"--dry-run")
            dryRun = true;
    }

    LocalFree(arguments);

    if (displayName.empty())
    {
        // Nothing to do, e.g. when the helper is invoked without arguments.
        return 0;
    }

    const auto displayNameLower = toLower(displayName);
    const auto keepProviderKeyLower = toLower(keepProviderKey);

    std::vector<StaleBundle> stale;
    collectStaleBundles(displayNameLower, keepProviderKeyLower, KEY_WOW64_64KEY, stale);
    collectStaleBundles(displayNameLower, keepProviderKeyLower, KEY_WOW64_32KEY, stale);

    for (const auto& bundle: stale)
    {
        log(L"%s stale registration %s (provider %s)",
            dryRun ? L"Would remove" : L"Removing", bundle.keyName.c_str(),
            bundle.providerKey.c_str());

        if (!dryRun)
            removeStaleBundle(bundle);
    }

    return 0;
}
