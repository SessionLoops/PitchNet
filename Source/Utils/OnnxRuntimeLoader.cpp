#include "OnnxRuntimeLoader.h"

#if defined(_WIN32) && defined(HAVE_ONNXRUNTIME)

#include <windows.h>
#include <cstring>
#include <delayimp.h>
#include <mutex>
#include <string>

namespace
{
    HMODULE loadedOnnxRuntime = nullptr;

    std::wstring getCurrentModuleDirectory()
    {
        HMODULE module = nullptr;
        const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;

        if (!GetModuleHandleExW(flags,
                                reinterpret_cast<LPCWSTR>(&getCurrentModuleDirectory),
                                &module) ||
            module == nullptr)
        {
            return {};
        }

        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const auto length = GetModuleFileNameW(module, path.data(),
                                                   static_cast<DWORD>(path.size()));
            if (length == 0)
                return {};

            if (length < path.size() - 1)
            {
                path.resize(length);
                break;
            }

            path.resize(path.size() * 2);
        }

        const auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return {};

        path.resize(slash);
        return path;
    }

    HMODULE loadOnnxRuntimeFromLocalDirectory()
    {
        const auto moduleDirectory = getCurrentModuleDirectory();
        if (moduleDirectory.empty())
            return nullptr;

        const auto dllPath = moduleDirectory + L"\\onnxruntime.dll";
        if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            return nullptr;

        const auto handle = LoadLibraryW(dllPath.c_str());
        if (handle == nullptr)
            return nullptr;

        return handle;
    }

    FARPROC WINAPI delayLoadHook(unsigned dliNotify, PDelayLoadInfo delayInfo)
    {
        if (dliNotify == dliNotePreLoadLibrary &&
            delayInfo != nullptr &&
            delayInfo->szDll != nullptr &&
            _stricmp(delayInfo->szDll, "onnxruntime.dll") == 0)
        {
            OnnxRuntimeLoader::ensureLoadedFromLocalDirectory();
            return reinterpret_cast<FARPROC>(loadedOnnxRuntime);
        }

        return nullptr;
    }
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = delayLoadHook;

namespace OnnxRuntimeLoader
{
    void ensureLoadedFromLocalDirectory()
    {
        static std::once_flag once;
        std::call_once(once, []()
        {
            loadedOnnxRuntime = loadOnnxRuntimeFromLocalDirectory();
        });
    }
}

#else

namespace OnnxRuntimeLoader
{
    void ensureLoadedFromLocalDirectory() {}
}

#endif
