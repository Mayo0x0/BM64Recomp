// Updates from the GitHub releases of this fork; see include/updater.h.
// Check: GET the latest release from the GitHub API and compare its tag ("v1.5.0") with the project version.
// Install: download the release's Windows zip and write each file next to the exe. A file that is there already is
// renamed to <name>.update-old first (Windows lets a running exe or a loaded dll be renamed, not overwritten); a
// failure puts every renamed file back. The user's own levels are kept. cleanup() removes the renamed files at the
// next start, which the launcher does right away (restart::relaunch).

#include "updater.h"

#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "json/json.hpp"
#include "miniz.h"
#include "miniz_zip.h"
#include "librecomp/game.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace {
constexpr const char* LATEST_URL = "https://api.github.com/repos/Mayo0x0/BM64Recomp/releases/latest";
constexpr const char* DOWNLOAD_PREFIX = "https://github.com/Mayo0x0/BM64Recomp/releases/download/";
constexpr const char* ASSET_NAME = "BM64Recompiled-Windows-Release.zip";
constexpr const char* OLD_SUFFIX = ".update-old";

std::mutex mutex;
updater::Status current;
std::string download_url;

void set_phase(updater::Phase phase, const std::string& error = "") {
    std::lock_guard lock(mutex);
    current.phase = phase;
    current.error = error;
    printf("UPDATE phase %d %s\n", (int)phase, error.c_str());
    fflush(stdout);
}

std::filesystem::path program_folder() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
#else
    return std::filesystem::current_path();
#endif
}

#ifdef _WIN32
// HTTPS GET; follows redirects (the zip is served from another host). progress(read, total), total 0 = unknown.
bool http_get(const std::string& url, std::vector<uint8_t>& out, std::string& error,
              const std::function<void(size_t, size_t)>& progress = nullptr) {
    std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "Bad address: " + url;
        return false;
    }
    HINTERNET session = WinHttpOpen(L"BM64Recompiled-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) { // before Windows 8.1
        session = WinHttpOpen(L"BM64Recompiled-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    }
    HINTERNET connection = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = request != nullptr &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1L,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
    }
    if (!ok) {
        error = "No connection to GitHub (error " + std::to_string(GetLastError()) + ").";
    } else if (status != 200) {
        error = "GitHub answered " + std::to_string(status) + ".";
        ok = false;
    } else {
        DWORD total = 0;
        size = sizeof(total);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &total, &size, WINHTTP_NO_HEADER_INDEX);
        out.clear();
        uint8_t buffer[65536];
        DWORD read = 0;
        while ((ok = WinHttpReadData(request, buffer, sizeof(buffer), &read)) && read > 0) {
            out.insert(out.end(), buffer, buffer + read);
            if (progress) progress(out.size(), total);
        }
        if (!ok) error = "The download broke off.";
        else if (total != 0 && out.size() != total) { error = "The download is incomplete."; ok = false; }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}
#endif

void run_check() {
#ifdef _WIN32
    std::vector<uint8_t> body;
    std::string error;
    if (!http_get(LATEST_URL, body, error)) {
        set_phase(updater::Phase::Failed, error);
        return;
    }
    nlohmann::json release = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
    if (release.is_discarded() || !release.contains("tag_name")) {
        set_phase(updater::Phase::Failed, "GitHub's answer could not be read.");
        return;
    }
    std::string tag = release.value("tag_name", "");
    std::string version = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    recomp::Version latest{};
    if (!recomp::Version::from_string(version, latest)) {
        set_phase(updater::Phase::Failed, "Unknown release name: " + tag);
        return;
    }
    std::string url;
    for (const auto& asset : release.value("assets", nlohmann::json::array())) {
        if (asset.value("name", "") == ASSET_NAME) url = asset.value("browser_download_url", "");
    }
    recomp::Version mine = recomp::get_project_version();
    if (const char* test = std::getenv("BM64_UPDATE_TEST")) recomp::Version::from_string(test, mine); // test: pretend this version
    bool newer = latest > mine;
    {
        std::lock_guard lock(mutex);
        current.latest = latest.to_string();
        current.title = release.value("name", tag);
        download_url = url.rfind(DOWNLOAD_PREFIX, 0) == 0 ? url : "";
    }
    if (newer && url.rfind(DOWNLOAD_PREFIX, 0) != 0) {
        set_phase(updater::Phase::Failed, "Release " + tag + " has no " + ASSET_NAME + ".");
        return;
    }
    printf("UPDATE latest release %s, this is %s\n", tag.c_str(), recomp::get_project_version().to_string().c_str()); fflush(stdout);
    set_phase(newer ? updater::Phase::Available : updater::Phase::UpToDate);
#endif
}

// Writes the zip's files into folder; on failure everything renamed is put back.
bool install_zip(const std::vector<uint8_t>& zip_data, const std::filesystem::path& folder, std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zip_data.data(), zip_data.size(), 0)) {
        error = "The download is not a valid zip.";
        return false;
    }
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moved; // (file, its old copy)
    std::vector<std::filesystem::path> added;
    bool ok = true;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && ok; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char name_buffer[1024];
        mz_zip_reader_get_filename(&zip, i, name_buffer, sizeof(name_buffer));
        std::string name = name_buffer;
        // Only plain relative paths: nothing may land outside the program's folder.
        if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos ||
            name.find("..") != std::string::npos) {
            error = "Unsafe path in the zip: " + name;
            ok = false;
            break;
        }
        std::filesystem::path target = folder / std::filesystem::u8path(name);
        std::error_code ec;
        bool exists = std::filesystem::exists(target, ec);
        if (exists && name.rfind("levels/", 0) == 0) continue; // the user's own copy of an example level
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (data == nullptr) {
            error = "Could not unpack " + name + ".";
            ok = false;
            break;
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        std::filesystem::path fresh = target;
        fresh += ".update-new";
        {
            std::ofstream file(fresh, std::ios::binary | std::ios::trunc);
            file.write((const char*)data, (std::streamsize)size);
            ok = file.good();
        }
        mz_free(data);
        if (!ok) {
            error = "Could not write " + name + ".";
            std::filesystem::remove(fresh, ec);
            break;
        }
        if (exists) {
            std::filesystem::path old = target;
            old += OLD_SUFFIX;
            std::filesystem::remove(old, ec);
            std::filesystem::rename(target, old, ec);
            if (ec) {
                error = "Could not replace " + name + ": " + ec.message();
                std::filesystem::remove(fresh, ec);
                ok = false;
                break;
            }
            moved.push_back({ target, old });
        } else {
            added.push_back(target);
        }
        std::filesystem::rename(fresh, target, ec);
        if (ec) {
            error = "Could not write " + name + ": " + ec.message();
            ok = false;
        }
    }
    mz_zip_reader_end(&zip);
    if (!ok) {
        std::error_code ec;
        for (const auto& path : added) std::filesystem::remove(path, ec);
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::filesystem::remove(it->first, ec);
            std::filesystem::rename(it->second, it->first, ec);
        }
    }
    return ok;
}

void run_install() {
#ifdef _WIN32
    std::string url;
    {
        std::lock_guard lock(mutex);
        url = download_url;
        current.percent = 0;
    }
    set_phase(updater::Phase::Downloading);
    std::vector<uint8_t> zip;
    std::string error;
    bool ok = http_get(url, zip, error, [](size_t read, size_t total) {
        std::lock_guard lock(mutex);
        current.percent = total ? (int)(read * 100 / total) : 0;
    });
    if (!ok) {
        set_phase(updater::Phase::Failed, error);
        return;
    }
    set_phase(updater::Phase::Installing);
    if (!install_zip(zip, program_folder(), error)) {
        set_phase(updater::Phase::Failed, error);
        return;
    }
    printf("UPDATE installed %s\n", url.c_str());
    set_phase(updater::Phase::Done);
#endif
}

// Detached: a joinable thread left at exit would end the program with std::terminate.
void start(void (*work)()) {
    std::thread(work).detach();
}
}

void updater::cleanup() {
    std::error_code ec;
    std::vector<std::filesystem::path> old_files;
    for (auto it = std::filesystem::recursive_directory_iterator(program_folder(), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::wstring name = it->path().filename().wstring();
        if (name.size() > 11 && name.ends_with(L".update-old")) old_files.push_back(it->path()); // OLD_SUFFIX
    }
    for (const auto& path : old_files) std::filesystem::remove(path, ec);
}

bool updater::supported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void updater::check() {
    {
        std::lock_guard lock(mutex);
        if (!supported() || current.phase == Phase::Checking || current.phase == Phase::Downloading ||
            current.phase == Phase::Installing || current.phase == Phase::Done) return;
        current.phase = Phase::Checking;
    }
    start(run_check);
}

void updater::install() {
    {
        std::lock_guard lock(mutex);
        if (current.phase != Phase::Available) return;
        current.phase = Phase::Downloading;
    }
    start(run_install);
}

updater::Status updater::status() {
    std::lock_guard lock(mutex);
    return current;
}
