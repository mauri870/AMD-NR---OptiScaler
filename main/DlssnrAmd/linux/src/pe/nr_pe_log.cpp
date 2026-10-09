#include "nr_pe_log.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace nr::pe {
namespace {

std::mutex log_lock;
HMODULE self_module = nullptr;

// Where this module is, by whichever route works. The loader's own handle first;
// deriving one from an address inside us second, because that is what the code
// used to do and it is right when it works; the executable's own folder last,
// which is where a drop-in module was copied to anyway.
std::string folder_of(HMODULE module) {
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(module, path, MAX_PATH)) return {};
    std::string s = path;
    const auto slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : s.substr(0, slash);
}

std::string resolve_folder() {
    HMODULE self = self_module;
    if (!self)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&resolve_folder), &self);
    if (self) {
        std::string folder = folder_of(self);
        if (!folder.empty()) return folder;
    }
    // Last resort: the executable's own directory, which is where a drop-in
    // module was copied to in the first place.
    return folder_of(nullptr);
}

std::string resolve_path() {
    const std::string folder = resolve_folder();
    return folder.empty() ? std::string("dlssnr-amd.log")
                          : folder + "\\dlssnr-amd.log";
}

FILE* file() {
    static std::string path = resolve_path();
    static FILE* handle = std::fopen(path.c_str(), "a");
    return handle;
}

}  // namespace

void set_module(void* module) { self_module = static_cast<HMODULE>(module); }

const char* module_folder() {
    static std::string folder = resolve_folder();
    return folder.c_str();
}

const char* log_path() {
    static std::string path = resolve_path();
    return path.c_str();
}

void log(const char* format, ...) {
    std::lock_guard<std::mutex> guard(log_lock);
    FILE* handle = file();
    if (!handle) return;
    va_list args; va_start(args, format);
    std::vfprintf(handle, format, args);
    va_end(args);
    std::fputc('\n', handle);
    std::fflush(handle);
}

}  // namespace nr::pe
