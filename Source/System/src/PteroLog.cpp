#include "System/PteroLog.h"

#include <windows.h>
#include <share.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <exception>
#include <mutex>

namespace
{
    // Large enough that a minute of chatty per-frame tracing still leaves the
    // startup banner visible in the Console, small enough that the snapshot the
    // panel takes each frame stays trivial.
    constexpr std::size_t kMaxRingEntries = 20000;

    // Ordinary traffic is buffered; this is how long a line may sit unflushed.
    constexpr std::chrono::milliseconds kFlushInterval{ 200 };

    // Sessions kept in Logs/ before the oldest are deleted on startup.
    constexpr std::size_t kMaxSessionFiles = 30;

    struct LogState
    {
        std::recursive_mutex        Mutex;
        std::deque<PteroLog::Entry> Ring;
        std::vector<std::string>    Categories;

        std::uint64_t Total = 0;
        std::uint64_t Evicted = 0;

        std::FILE*   File = nullptr;
        std::wstring FilePath;
        std::string  FilePathUtf8;

        std::chrono::steady_clock::time_point Start;
        std::chrono::steady_clock::time_point LastFlush;

        std::atomic<PteroLog::Level> Minimum{ PteroLog::Level::Debug };

        bool Initialized = false;
        bool ShutDown = false;

        LPTOP_LEVEL_EXCEPTION_FILTER PreviousFilter = nullptr;
        std::terminate_handler       PreviousTerminate = nullptr;
    };

    // Function-local so the crash handlers can still reach it during static
    // destruction, which is exactly when a shutdown fault would fire.
    LogState& State()
    {
        static LogState state;
        return state;
    }

    const char* const kLevelNames[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

    std::string Utf8FromWide(const std::wstring& wide)
    {
        if (wide.empty()) return {};
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                                 nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<std::size_t>(needed), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                              out.data(), needed, nullptr, nullptr);
        return out;
    }

    // Creates every missing component of `path`, not just the leaf.
    void CreateDirectoryTree(const std::wstring& path)
    {
        if (path.empty()) return;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            if (path[i] == L'\\' || path[i] == L'/')
            {
                if (i > 0) ::CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
            }
        }
        ::CreateDirectoryW(path.c_str(), nullptr);
    }

    // <exe dir>/Logs, or <exe dir>/../Logs when the exe sits in Binaries - which
    // is where this engine ships it, so the usual answer is <repo>/Logs.
    std::wstring DefaultLogDirectory()
    {
        wchar_t modulePath[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            return L"Logs";

        std::wstring directory(modulePath);
        const std::size_t slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return L"Logs";
        directory.resize(slash);

        const std::size_t parentSlash = directory.find_last_of(L"\\/");
        if (parentSlash != std::wstring::npos)
        {
            std::wstring leaf = directory.substr(parentSlash + 1);
            std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (leaf == L"binaries" || leaf == L"debug" || leaf == L"release" || leaf == L"x64")
                directory.resize(parentSlash);
        }

        return directory + L"\\Logs";
    }

    // Keeps the newest kMaxSessionFiles and deletes the rest, so an engine that
    // is restarted fifty times a day does not turn Logs/ into an archive.
    void PruneOldSessions(const std::wstring& directory)
    {
        struct Found { std::wstring Name; FILETIME Written; };
        std::vector<Found> found;

        WIN32_FIND_DATAW data{};
        const std::wstring pattern = directory + L"\\Ptero_*.txt";
        HANDLE handle = ::FindFirstFileW(pattern.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE) return;
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                found.push_back({ data.cFileName, data.ftLastWriteTime });
        } while (::FindNextFileW(handle, &data));
        ::FindClose(handle);

        if (found.size() < kMaxSessionFiles) return;

        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b)
        {
            return ::CompareFileTime(&a.Written, &b.Written) > 0;   // newest first
        });

        for (std::size_t i = kMaxSessionFiles - 1; i < found.size(); ++i)
            ::DeleteFileW((directory + L"\\" + found[i].Name).c_str());
    }

    std::wstring SessionFileName()
    {
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        wchar_t buffer[64] = {};
        std::swprintf(buffer, 64, L"Ptero_%04u-%02u-%02u_%02u-%02u-%02u.txt",
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        return buffer;
    }

    void WallClockStamp(char* out, std::size_t capacity)
    {
        SYSTEMTIME now{};
        ::GetLocalTime(&now);
        std::snprintf(out, capacity, "%02u:%02u:%02u.%03u",
                      now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    }

    // Caller holds the lock.
    void FlushLocked(LogState& state, bool force)
    {
        if (state.File == nullptr) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - state.LastFlush < kFlushInterval) return;
        std::fflush(state.File);
        state.LastFlush = now;
    }

    // Writes one already-formatted line to every sink. Caller holds the lock.
    void EmitLocked(LogState& state, PteroLog::Level level, const char* category, const char* message)
    {
        const int levelIndex = static_cast<int>(level);
        const char* levelName = (levelIndex >= 0 && levelIndex < 6) ? kLevelNames[levelIndex] : "INFO";
        const char* categoryName = (category != nullptr && *category != '\0') ? category : "General";

        PteroLog::Entry entry;
        entry.Sequence = ++state.Total;
        entry.TimeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.Start).count();
        entry.MessageLevel = level;
        entry.Category = categoryName;
        entry.Message = (message != nullptr) ? message : "";

        if (std::find(state.Categories.begin(), state.Categories.end(), entry.Category) == state.Categories.end())
            state.Categories.push_back(entry.Category);

        if (state.File != nullptr)
        {
            char stamp[32] = {};
            WallClockStamp(stamp, sizeof(stamp));
            std::fprintf(state.File, "[%s] [%-5s] [%-12s] %s\n",
                         stamp, levelName, categoryName, entry.Message.c_str());

            // Anything the user would want to read after a crash is worth a
            // syscall now, because there may not be a later.
            FlushLocked(state, level >= PteroLog::Level::Warning);
        }

        {
            std::string debugLine = "[";
            debugLine += categoryName;
            debugLine += "] ";
            debugLine += entry.Message;
            debugLine += "\n";
            ::OutputDebugStringA(debugLine.c_str());
        }

        state.Ring.push_back(std::move(entry));
        while (state.Ring.size() > kMaxRingEntries)
        {
            state.Ring.pop_front();
            ++state.Evicted;
        }
    }

    const char* ExceptionName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        default:                                 return "unknown exception";
        }
    }

    LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
    {
        LogState& state = State();

        if (info != nullptr && info->ExceptionRecord != nullptr)
        {
            const EXCEPTION_RECORD& record = *info->ExceptionRecord;
            PteroLog::Writef(PteroLog::Level::Fatal, "Crash",
                             "Unhandled %s (0x%08X) at address 0x%p.",
                             ExceptionName(record.ExceptionCode),
                             static_cast<unsigned>(record.ExceptionCode),
                             record.ExceptionAddress);

            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            {
                const ULONG_PTR operation = record.ExceptionInformation[0];
                PteroLog::Writef(PteroLog::Level::Fatal, "Crash",
                                 "Access violation %s address 0x%p.",
                                 operation == 0 ? "reading" : (operation == 1 ? "writing" : "executing"),
                                 reinterpret_cast<void*>(record.ExceptionInformation[1]));
            }
        }
        else
        {
            PteroLog::Write(PteroLog::Level::Fatal, "Crash", "Unhandled exception with no record.");
        }

        PteroLog::Shutdown("crashed");

        return (state.PreviousFilter != nullptr) ? state.PreviousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }

    void CrashTerminate()
    {
        PteroLog::Write(PteroLog::Level::Fatal, "Crash", "std::terminate called - unhandled C++ exception.");
        PteroLog::Shutdown("terminated");

        LogState& state = State();
        if (state.PreviousTerminate != nullptr) state.PreviousTerminate();
        std::abort();
    }

    // Fires on logoff and shutdown, and on Ctrl+C when a console is attached.
    BOOL WINAPI ConsoleCtrlHandler(DWORD type)
    {
        const char* reason = "console control event";
        switch (type)
        {
        case CTRL_C_EVENT:        reason = "Ctrl+C";          break;
        case CTRL_BREAK_EVENT:    reason = "Ctrl+Break";      break;
        case CTRL_CLOSE_EVENT:    reason = "console closed";  break;
        case CTRL_LOGOFF_EVENT:   reason = "logoff";          break;
        case CTRL_SHUTDOWN_EVENT: reason = "system shutdown"; break;
        default: break;
        }
        PteroLog::Shutdown(reason);
        return FALSE;   // let the default handler run
    }

    void OnExit()
    {
        PteroLog::Shutdown("process exit");
    }
}

namespace PteroLog
{
    void Initialize(const wchar_t* logDirectory)
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        if (state.Initialized) return;

        state.Initialized = true;
        state.ShutDown = false;
        state.Start = std::chrono::steady_clock::now();
        state.LastFlush = state.Start;

        const std::wstring directory = (logDirectory != nullptr && *logDirectory != L'\0')
            ? std::wstring(logDirectory)
            : DefaultLogDirectory();

        CreateDirectoryTree(directory);
        PruneOldSessions(directory);

        state.FilePath = directory + L"\\" + SessionFileName();

        // A second editor started in the same second would collide; walk a
        // suffix rather than silently sharing a handle.
        for (int attempt = 1; attempt < 100; ++attempt)
        {
            if (::GetFileAttributesW(state.FilePath.c_str()) == INVALID_FILE_ATTRIBUTES) break;
            std::wstring candidate = directory + L"\\" + SessionFileName();
            candidate.resize(candidate.size() - 4);     // strip ".txt"
            wchar_t suffix[16] = {};
            std::swprintf(suffix, 16, L"_%d.txt", attempt);
            state.FilePath = candidate + suffix;
        }

        state.File = ::_wfsopen(state.FilePath.c_str(), L"wt", _SH_DENYWR);
        state.FilePathUtf8 = Utf8FromWide(state.FilePath);

        state.PreviousFilter = ::SetUnhandledExceptionFilter(CrashFilter);
        state.PreviousTerminate = std::set_terminate(CrashTerminate);
        ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        std::atexit(OnExit);

        if (state.File != nullptr)
        {
            SYSTEMTIME now{};
            ::GetLocalTime(&now);
            std::fprintf(state.File,
                         "Ptero Engine session log\n"
                         "Started %04u-%02u-%02u %02u:%02u:%02u\n"
                         "File    %s\n"
                         "----------------------------------------------------------------------\n",
                         now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                         state.FilePathUtf8.c_str());
            std::fflush(state.File);
        }

        EmitLocked(state, Level::Info, "Log",
                   state.File != nullptr ? "Logging to disk started." : "Could not open a session log file.");
        if (state.File != nullptr)
            EmitLocked(state, Level::Info, "Log", state.FilePathUtf8.c_str());
    }

    void Shutdown(const char* reason)
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        if (!state.Initialized || state.ShutDown) return;
        state.ShutDown = true;

        char message[256] = {};
        std::snprintf(message, sizeof(message), "Session ended (%s). %llu lines written.",
                      (reason != nullptr) ? reason : "unspecified",
                      static_cast<unsigned long long>(state.Total));
        EmitLocked(state, Level::Info, "Log", message);

        if (state.File != nullptr)
        {
            std::fprintf(state.File,
                         "----------------------------------------------------------------------\n");
            std::fflush(state.File);
            std::fclose(state.File);
            state.File = nullptr;
        }
    }

    void Write(Level level, const char* category, const char* message)
    {
        if (level < State().Minimum.load(std::memory_order_relaxed)) return;
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        EmitLocked(state, level, category, message);
    }

    void WriteV(Level level, const char* category, const char* format, va_list args)
    {
        if (level < State().Minimum.load(std::memory_order_relaxed)) return;

        char stackBuffer[1024];
        va_list copy;
        va_copy(copy, args);
        const int needed = std::vsnprintf(stackBuffer, sizeof(stackBuffer), format, copy);
        va_end(copy);

        if (needed < 0)
        {
            Write(level, category, format);
            return;
        }

        if (static_cast<std::size_t>(needed) < sizeof(stackBuffer))
        {
            Write(level, category, stackBuffer);
            return;
        }

        std::string heapBuffer(static_cast<std::size_t>(needed) + 1, '\0');
        std::vsnprintf(heapBuffer.data(), heapBuffer.size(), format, args);
        heapBuffer.resize(static_cast<std::size_t>(needed));
        Write(level, category, heapBuffer.c_str());
    }

    void Writef(Level level, const char* category, const char* format, ...)
    {
        if (level < State().Minimum.load(std::memory_order_relaxed)) return;
        va_list args;
        va_start(args, format);
        WriteV(level, category, format, args);
        va_end(args);
    }

    std::size_t Snapshot(std::vector<Entry>& outEntries, std::size_t maxEntries)
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);

        const std::size_t count = (std::min)(maxEntries, state.Ring.size());
        outEntries.clear();
        outEntries.reserve(count);
        for (std::size_t i = state.Ring.size() - count; i < state.Ring.size(); ++i)
            outEntries.push_back(state.Ring[i]);
        return count;
    }

    std::uint64_t TotalCount()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        return state.Total;
    }

    std::uint64_t EvictedCount()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        return state.Evicted;
    }

    void Clear()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        // Only the on-screen ring is cleared. The file is the record of what
        // happened, and clearing the Console must not edit history.
        state.Ring.clear();
        state.Evicted = 0;
    }

    void Flush()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        FlushLocked(state, true);
    }

    Level MinimumLevel()
    {
        return State().Minimum.load(std::memory_order_relaxed);
    }

    void SetMinimumLevel(Level level)
    {
        State().Minimum.store(level, std::memory_order_relaxed);
    }

    std::wstring SessionFilePath()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        return state.FilePath;
    }

    std::string SessionFilePathUtf8()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        return state.FilePathUtf8;
    }

    const char* LevelName(Level level)
    {
        const int index = static_cast<int>(level);
        return (index >= 0 && index < 6) ? kLevelNames[index] : "INFO";
    }

    std::vector<std::string> Categories()
    {
        LogState& state = State();
        std::lock_guard<std::recursive_mutex> lock(state.Mutex);
        return state.Categories;
    }
}
