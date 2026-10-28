#include "pch.h"

#include "DataFolderWatcher.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace DataFolderWatcher
{
    namespace
    {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;

        // Large enough that unpacking a whole asset pack rarely overflows it; on overflow
        // the folder is rescanned instead.
        constexpr DWORD kBufferBytes = 1u << 20;
        // How long a file must stay untouched before it counts as fully written.
        constexpr auto kSettleTime = std::chrono::milliseconds(1500);

        struct State
        {
            std::mutex Mutex;
            std::thread Thread;
            HANDLE StopEvent = nullptr;
            fs::path Directory;
            Classifier Classify;
            std::vector<DetectedFile> Detected;
            std::set<std::wstring> Ignored;
            std::atomic<bool> Changed{ false };
        };

        // Never destroyed: a joinable std::thread in a static destructor would terminate
        // the process, and joining at DLL unload deadlocks on the loader lock. The thread
        // simply ends with the process.
        State& GetState()
        {
            static State* state = new State();
            return *state;
        }

        std::wstring Key(const fs::path& path)
        {
            std::wstring key = path.lexically_normal().wstring();
            for (wchar_t& c : key)
            {
                c = static_cast<wchar_t>(towlower(c));
            }
            return key;
        }

        // A writer that still has the file open (a copy in progress) denies read-only
        // sharing, so this fails until the copy completes.
        bool IsReadable(const fs::path& path)
        {
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            CloseHandle(file);
            return true;
        }

        void AddTree(const fs::path& root, std::map<std::wstring, std::pair<fs::path, Clock::time_point>>& pending)
        {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
            {
                if (ec)
                {
                    break;
                }
                if (it->is_regular_file(ec))
                {
                    pending[Key(it->path())] = { it->path(), Clock::now() };
                }
            }
        }

        void Run(State& state, fs::path directory, HANDLE stopEvent, Classifier classify)
        {
            const HANDLE handle = CreateFileW(
                directory.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return;
            }

            std::vector<DWORD> buffer(kBufferBytes / sizeof(DWORD));
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::map<std::wstring, std::pair<fs::path, Clock::time_point>> pending;
            bool readIssued = false;

            constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

            for (;;)
            {
                if (!readIssued)
                {
                    ResetEvent(overlapped.hEvent);
                    if (!ReadDirectoryChangesW(handle, buffer.data(), kBufferBytes, TRUE, kFilter, nullptr, &overlapped, nullptr))
                    {
                        break;
                    }
                    readIssued = true;
                }

                const HANDLE waits[2] = { overlapped.hEvent, stopEvent };
                const DWORD signalled = WaitForMultipleObjects(2, waits, FALSE, 500);
                if (signalled == WAIT_OBJECT_0 + 1)
                {
                    break;
                }

                if (signalled == WAIT_OBJECT_0)
                {
                    readIssued = false;
                    DWORD bytes = 0;
                    if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE))
                    {
                        continue;
                    }
                    state.Changed.store(true);

                    if (bytes == 0)
                    {
                        // The buffer overflowed and the individual changes are lost:
                        // look at everything; the classifier skips what is imported.
                        AddTree(directory, pending);
                        continue;
                    }

                    const BYTE* cursor = reinterpret_cast<const BYTE*>(buffer.data());
                    for (;;)
                    {
                        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor);
                        const fs::path path = directory / std::wstring(info->FileName, info->FileNameLength / sizeof(WCHAR));
                        if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME ||
                            info->Action == FILE_ACTION_MODIFIED)
                        {
                            std::error_code ec;
                            if (fs::is_directory(path, ec))
                            {
                                // A folder moved in from the same drive arrives as one event;
                                // its contents never report individually.
                                if (info->Action != FILE_ACTION_MODIFIED)
                                {
                                    AddTree(path, pending);
                                }
                            }
                            else
                            {
                                pending[Key(path)] = { path, Clock::now() };
                            }
                        }
                        if (info->NextEntryOffset == 0)
                        {
                            break;
                        }
                        cursor += info->NextEntryOffset;
                    }
                }

                // Hand over files that have settled.
                const Clock::time_point now = Clock::now();
                std::vector<fs::path> settled;
                for (auto it = pending.begin(); it != pending.end();)
                {
                    auto& [path, lastChange] = it->second;
                    if (now - lastChange < kSettleTime)
                    {
                        ++it;
                        continue;
                    }
                    std::error_code ec;
                    if (!fs::is_regular_file(path, ec))
                    {
                        it = pending.erase(it);
                        continue;
                    }
                    if (!IsReadable(path))
                    {
                        lastChange = now;   // still being written
                        ++it;
                        continue;
                    }
                    settled.push_back(path);
                    it = pending.erase(it);
                }

                for (const fs::path& path : settled)
                {
                    {
                        std::lock_guard<std::mutex> lock(state.Mutex);
                        if (state.Ignored.count(Key(path)) != 0)
                        {
                            continue;
                        }
                    }
                    std::string kind;
                    if (classify && classify(path, kind))
                    {
                        std::lock_guard<std::mutex> lock(state.Mutex);
                        state.Detected.push_back({ path, kind });
                    }
                }
            }

            if (readIssued)
            {
                CancelIoEx(handle, &overlapped);
                DWORD bytes = 0;
                GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
            }
            CloseHandle(overlapped.hEvent);
            CloseHandle(handle);
        }
    }

    void Start(const std::filesystem::path& dataDirectory, Classifier classifier)
    {
        State& state = GetState();
        if (dataDirectory.empty())
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state.Mutex);
            if (state.Thread.joinable() && Key(state.Directory) == Key(dataDirectory))
            {
                return;
            }
        }

        Stop();

        std::lock_guard<std::mutex> lock(state.Mutex);
        state.Directory = dataDirectory;
        state.Classify = std::move(classifier);
        state.StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state.Thread = std::thread(Run, std::ref(state), dataDirectory, state.StopEvent, state.Classify);
    }

    void Stop()
    {
        State& state = GetState();
        std::thread thread;
        HANDLE stopEvent = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.Mutex);
            thread = std::move(state.Thread);
            stopEvent = state.StopEvent;
            state.StopEvent = nullptr;
        }
        if (stopEvent != nullptr)
        {
            SetEvent(stopEvent);
        }
        if (thread.joinable())
        {
            thread.join();
        }
        if (stopEvent != nullptr)
        {
            CloseHandle(stopEvent);
        }
    }

    std::vector<DetectedFile> TakeDetectedFiles()
    {
        State& state = GetState();
        std::lock_guard<std::mutex> lock(state.Mutex);
        std::vector<DetectedFile> detected;
        detected.swap(state.Detected);
        return detected;
    }

    bool ConsumeChanged()
    {
        return GetState().Changed.exchange(false);
    }

    void Ignore(const std::vector<std::filesystem::path>& files)
    {
        State& state = GetState();
        std::lock_guard<std::mutex> lock(state.Mutex);
        for (const std::filesystem::path& file : files)
        {
            state.Ignored.insert(Key(file));
        }
    }
}
