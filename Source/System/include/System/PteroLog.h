#pragma once

// ---------------------------------------------------------------------------
// PteroLog - the engine's single log sink.
//
// Everything written here goes three places at once:
//
//   * an in-memory ring, which the editor's Console panel renders,
//   * a session .txt under <repo>/Logs, and
//   * OutputDebugStringA, so an attached debugger still sees the stream.
//
// The file is the reason this exists. A renderer bug that only shows up after
// several minutes of editing is worth nothing if the evidence dies with the
// process, so the sink is written through: buffered for ordinary traffic and
// flushed outright for anything at Warning or above, plus once per frame from
// the render loop. A process killed from Task Manager therefore loses at most
// the current frame's lines - and a process that faults writes its own
// stack-free epitaph through the unhandled-exception filter installed by
// Initialize.
//
// Thread-safe: the renderer logs from the frame thread, asset loading from
// worker threads, and the crash handlers from whichever thread died.
//
// Lives in System because it is not a renderer facility - asset import, the
// node graph and the editor shell all have things to say. Following this
// project's convention, System sources are compiled into their consumers
// rather than linked across a DLL boundary, so each module that includes this
// gets its own sink. Today only Renderer_DX12.dll runs one in the editor
// process; if System.dll is ever loaded alongside it, the two would open
// separate session files and this has to move behind SYSTEM_ASSET_API first.
// ---------------------------------------------------------------------------

#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace PteroLog
{
    enum class Level : int
    {
        Trace = 0,
        Debug,
        Info,
        Warning,
        Error,
        Fatal,
        Count
    };

    // One line as the Console renders it. Category is a short tag ("RTGI",
    // "Editor", "Assets") used for filtering; it is never formatted into the
    // message so the filter stays exact.
    struct Entry
    {
        std::uint64_t Sequence = 0;
        double        TimeSeconds = 0.0;  // since Initialize
        Level         MessageLevel = Level::Info;
        std::string   Category;
        std::string   Message;
    };

    // Opens the session file and installs the crash handlers. Calling it twice
    // is a no-op, so it is safe to call from both the DLL and the host.
    // `logDirectory` may be null, in which case the directory is derived from
    // the running module: <exe dir>/../Logs when the exe sits in Binaries,
    // <exe dir>/Logs otherwise.
    void Initialize(const wchar_t* logDirectory = nullptr);

    // Writes the footer and closes the file. Safe to call when uninitialised.
    void Shutdown(const char* reason = "normal exit");

    void Write(Level level, const char* category, const char* message);
    void Writef(Level level, const char* category, const char* format, ...);
    void WriteV(Level level, const char* category, const char* format, va_list args);

    // Copies the most recent `maxEntries` lines, oldest first. Returns how many
    // were written. Cheap enough to call every frame at the ring's full size.
    std::size_t Snapshot(std::vector<Entry>& outEntries, std::size_t maxEntries);

    // Total lines ever written, and how many the ring has evicted. The Console
    // shows the difference so a flood is visible rather than silent.
    std::uint64_t TotalCount();
    std::uint64_t EvictedCount();

    void Clear();
    void Flush();

    // Lines below this level are discarded before they reach the ring or the
    // file. Defaults to Debug; Trace has to be asked for.
    Level MinimumLevel();
    void  SetMinimumLevel(Level level);

    // Empty until Initialize has opened a file.
    std::wstring SessionFilePath();
    std::string  SessionFilePathUtf8();

    const char* LevelName(Level level);

    // Categories seen this session, in first-use order. The Console builds its
    // filter list from this.
    std::vector<std::string> Categories();
}

// The macros exist so a disabled level costs one comparison and no formatting.
#define PTERO_LOG_TRACE(category, ...) \
    do { if (::PteroLog::MinimumLevel() <= ::PteroLog::Level::Trace) \
         ::PteroLog::Writef(::PteroLog::Level::Trace, category, __VA_ARGS__); } while (0)
#define PTERO_LOG_DEBUG(category, ...) \
    do { if (::PteroLog::MinimumLevel() <= ::PteroLog::Level::Debug) \
         ::PteroLog::Writef(::PteroLog::Level::Debug, category, __VA_ARGS__); } while (0)
#define PTERO_LOG_INFO(category, ...)    ::PteroLog::Writef(::PteroLog::Level::Info,    category, __VA_ARGS__)
#define PTERO_LOG_WARNING(category, ...) ::PteroLog::Writef(::PteroLog::Level::Warning, category, __VA_ARGS__)
#define PTERO_LOG_ERROR(category, ...)   ::PteroLog::Writef(::PteroLog::Level::Error,   category, __VA_ARGS__)
#define PTERO_LOG_FATAL(category, ...)   ::PteroLog::Writef(::PteroLog::Level::Fatal,   category, __VA_ARGS__)
