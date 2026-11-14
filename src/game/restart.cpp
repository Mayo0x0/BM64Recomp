#include "restart.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "ultramodern/ultramodern.hpp"

namespace {
std::vector<std::string> arguments;

void set_variable(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str()); // an empty value removes it
#else
    if (value.empty()) {
        unsetenv(name.c_str());
    } else {
        setenv(name.c_str(), value.c_str(), 1);
    }
#endif
}
}

void restart::init(int argc, char** argv) {
    arguments.assign(argv, argv + argc);
    const char* parent = std::getenv("BM64_WAIT_PID");
    if (parent == nullptr) {
        return;
    }
    unsigned long pid = std::strtoul(parent, nullptr, 10);
    set_variable("BM64_WAIT_PID", "");
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (process != nullptr) {
        WaitForSingleObject(process, 15000);
        CloseHandle(process);
    }
#else
    for (int i = 0; i < 300 && kill((pid_t)pid, 0) == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

bool restart::start_game_at_once() {
    return std::getenv("BM64_RESTART") != nullptr;
}

void restart::relaunch(const std::vector<std::pair<std::string, std::string>>& environment) {
    for (const auto& [name, value] : environment) {
        set_variable(name, value);
    }
#ifdef _WIN32
    set_variable("BM64_WAIT_PID", std::to_string(GetCurrentProcessId()));
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    // The new process writes to the same output (a log file, when there is one), and inherits nothing else: an
    // inherited socket would keep netplay's port busy.
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    HANDLE handles[2] = { GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    DWORD handle_count = 0;
    for (HANDLE handle : handles) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_DISK &&
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            handles[handle_count++] = handle;
        }
    }
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
    std::vector<char> list_memory(list_size);
    auto list = (LPPROC_THREAD_ATTRIBUTE_LIST)list_memory.data();
    bool inherit = handle_count > 0 && InitializeProcThreadAttributeList(list, 1, 0, &list_size) &&
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, handle_count * sizeof(HANDLE), nullptr, nullptr);
    if (inherit) {
        startup.lpAttributeList = list;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = handles[0];
        startup.StartupInfo.hStdError = handles[handle_count - 1];
    }
    PROCESS_INFORMATION info{};
    std::wstring command_line = GetCommandLineW();
    if (CreateProcessW(path, command_line.data(), nullptr, nullptr, inherit, inherit ? EXTENDED_STARTUPINFO_PRESENT : 0,
                       nullptr, nullptr, &startup.StartupInfo, &info)) {
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
    }
#else
    set_variable("BM64_WAIT_PID", std::to_string(getpid()));
    if (!arguments.empty() && fork() == 0) {
        std::vector<char*> argv;
        for (std::string& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execv("/proc/self/exe", argv.data()); // Linux
        execvp(argv[0], argv.data());          // elsewhere
        _exit(1);
    }
#endif
    ultramodern::quit();
}
