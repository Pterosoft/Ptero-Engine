#include "VideoImport.h"

#include "VpxDecoder.h"
#include "WebmDemuxer.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{
    const wchar_t* const kConvertibleExtensions[] = {
        L".webm", L".mp4", L".m4v", L".mov", L".mkv", L".avi", L".wmv", L".mpg", L".mpeg",
        L".flv", L".ogv", L".3gp", L".ts", L".mts", L".m2ts", L".gif"
    };

    std::wstring Lower(std::wstring text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return text;
    }

    std::string Narrow(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::filesystem::path ModuleDirectory()
    {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module);
        wchar_t buffer[MAX_PATH * 4] = {};
        GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
        return std::filesystem::path(buffer).parent_path();
    }

    std::wstring Quote(const std::wstring& argument)
    {
        // Paths never end in a backslash here, so plain quoting is enough.
        return L"\"" + argument + L"\"";
    }

    // Runs a console program without a window. stdout is delivered line by line to
    // `onLine`; stderr goes to `stderrText` (the tail is what explains a failure).
    // Returns the exit code, or -1 if it could not start or was cancelled.
    int RunProcess(const std::wstring& exe, const std::wstring& arguments, const std::function<void(const std::string&)>& onLine,
                   std::string& stderrText, const std::atomic<bool>* cancel)
    {
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
        if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0))
        {
            return -1;
        }

        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullptr;
        startup.hStdOutput = outWrite;
        startup.hStdError = errWrite;

        // A job that kills its members when its last handle closes: an editor that exits
        // or crashes mid-import must not leave ffmpeg grinding away in the background.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }

        std::wstring commandLine = Quote(exe) + L" " + arguments;
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process);
        CloseHandle(outWrite);
        CloseHandle(errWrite);
        if (!started)
        {
            CloseHandle(outRead);
            CloseHandle(errRead);
            if (job != nullptr)
            {
                CloseHandle(job);
            }

            return -1;
        }

        if (job != nullptr)
        {
            AssignProcessToJobObject(job, process.hProcess);
        }

        ResumeThread(process.hThread);

        // stderr on its own thread: ffmpeg writes to both, and a full pipe nobody reads
        // would stall it.
        std::thread errorReader([&] {
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(errRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                stderrText.append(buffer, read);
                if (stderrText.size() > 64 * 1024)
                {
                    stderrText.erase(0, stderrText.size() - 32 * 1024);
                }
            }
        });

        std::thread canceller;
        std::atomic<bool> finished{ false };
        if (cancel != nullptr)
        {
            canceller = std::thread([&] {
                while (!finished.load())
                {
                    if (cancel->load())
                    {
                        TerminateProcess(process.hProcess, 1);
                        return;
                    }

                    Sleep(50);
                }
            });
        }

        std::string pending;
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(outRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        {
            pending.append(buffer, read);
            size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, newline);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }

                pending.erase(0, newline + 1);
                if (onLine)
                {
                    onLine(line);
                }
            }
        }

        WaitForSingleObject(process.hProcess, INFINITE);
        finished.store(true);
        if (canceller.joinable())
        {
            canceller.join();
        }

        errorReader.join();

        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(outRead);
        CloseHandle(errRead);
        if (job != nullptr)
        {
            CloseHandle(job);
        }

        if (cancel != nullptr && cancel->load())
        {
            return -1;
        }

        return static_cast<int>(exitCode);
    }

    double ProbeDuration(const std::wstring& ffmpeg, const std::wstring& source)
    {
        const std::filesystem::path ffprobe = std::filesystem::path(ffmpeg).parent_path() / L"ffprobe.exe";
        if (!std::filesystem::exists(ffprobe))
        {
            return 0.0;
        }

        double duration = 0.0;
        std::string ignored;
        RunProcess(ffprobe.wstring(),
                   L"-v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 " + Quote(source),
                   [&](const std::string& line) {
                       if (duration <= 0.0)
                       {
                           duration = std::strtod(line.c_str(), nullptr);
                       }
                   },
                   ignored, nullptr);
        return duration > 0.0 ? duration : 0.0;
    }

    // The last `count` non-empty lines joined into one, which is where ffmpeg explains why
    // it gave up.
    std::string LastLines(const std::string& text, size_t count)
    {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find_first_of("\r\n", start);
            if (end == std::string::npos)
            {
                end = text.size();
            }

            if (end > start)
            {
                lines.push_back(text.substr(start, end - start));
            }

            start = end + 1;
        }

        std::string tail;
        for (size_t i = lines.size() > count ? lines.size() - count : 0; i < lines.size(); ++i)
        {
            tail += (tail.empty() ? "" : " | ") + lines[i];
        }

        return tail.empty() ? std::string("no error output") : tail;
    }

    // Whether the engine can play the file as it is: VP8/VP9 in WebM, and a first frame
    // the decoder accepts (which rules out the 10/12-bit VP9 profiles).
    bool IsPlayableWebm(const std::wstring& path)
    {
        WebmDemuxer demuxer;
        std::string error;
        if (!demuxer.Open(path, error))
        {
            return false;
        }

        VpxDecoder decoder;
        if (!decoder.Open(demuxer.GetCodec(), error))
        {
            return false;
        }

        WebmDemuxer::Packet packet;
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        for (int attempt = 0; attempt < 8 && demuxer.ReadPacket(packet); ++attempt)
        {
            if (decoder.Decode(packet.Data, pixels, width, height, error))
            {
                return true;
            }

            if (!error.empty())
            {
                return false;
            }
        }

        return false;
    }
}

namespace VideoImport
{
    bool IsVideoFile(const std::wstring& path)
    {
        const std::wstring extension = Lower(std::filesystem::path(path).extension().wstring());
        for (const wchar_t* candidate : kConvertibleExtensions)
        {
            if (extension == candidate)
            {
                return true;
            }
        }

        return false;
    }

    const char* GetFileDialogPattern()
    {
        return "*.webm;*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv;*.mpg;*.mpeg;*.flv;*.ogv;*.3gp;*.ts;*.mts;*.m2ts;*.gif";
    }

    std::wstring FindFfmpeg()
    {
        const std::filesystem::path directory = ModuleDirectory();
        for (const std::filesystem::path& candidate :
             { directory / L"ffmpeg" / L"bin" / L"ffmpeg.exe", directory / L"ffmpeg" / L"ffmpeg.exe" })
        {
            if (std::filesystem::exists(candidate))
            {
                return candidate.wstring();
            }
        }

        wchar_t found[MAX_PATH] = {};
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0)
        {
            return found;
        }

        return {};
    }

    bool ImportIntoDirectory(const std::wstring& sourcePath,
                             const std::wstring& targetDirectory,
                             std::wstring& outputPath,
                             std::string& message,
                             const std::function<void(float)>& progress,
                             const std::atomic<bool>* cancel)
    {
        namespace fs = std::filesystem;
        std::error_code error;

        const fs::path source(sourcePath);
        if (!fs::is_regular_file(source, error))
        {
            message = "Source file not found.";
            return false;
        }

        if (!IsVideoFile(sourcePath))
        {
            message = "Not a supported video format.";
            return false;
        }

        fs::create_directories(targetDirectory, error);
        const fs::path target = fs::path(targetDirectory) / (source.stem().wstring() + L".webm");
        outputPath = target.wstring();

        // An existing VP8/VP9 WebM is taken as-is.
        if (Lower(source.extension().wstring()) == L".webm")
        {
            if (IsPlayableWebm(sourcePath))
            {
                if (fs::equivalent(source, target, error))
                {
                    message = "Already in the project: " + Narrow(target.filename().wstring());
                    return true;
                }

                if (!fs::copy_file(source, target, fs::copy_options::overwrite_existing, error))
                {
                    message = "Copy failed: " + error.message();
                    return false;
                }

                if (progress)
                {
                    progress(1.0f);
                }

                message = "Copied " + Narrow(target.filename().wstring()) + " (already WebM).";
                return true;
            }
        }

        const std::wstring ffmpeg = FindFfmpeg();
        if (ffmpeg.empty())
        {
            message = "ffmpeg.exe was not found (expected in Binaries\\ffmpeg\\bin).";
            return false;
        }

        const double duration = ProbeDuration(ffmpeg, sourcePath);

        // ffmpeg writes next to the result and the file is renamed into place only once it
        // is complete, so an aborted import never leaves a truncated video behind.
        const fs::path temporary = fs::path(targetDirectory) / (source.stem().wstring() + L".webm.importing");
        fs::remove(temporary, error);

        const unsigned threads = (std::min)((std::max)(std::thread::hardware_concurrency(), 1u), 16u);
        // VP9 at constant quality with 8-bit 4:2:0 output, which is what the engine's
        // decoder handles. A key frame at least every 4 seconds keeps seeking responsive.
        // The first audio track, if any, is kept as Opus.
        const std::wstring arguments =
            L"-hide_banner -nostdin -y -i " + Quote(sourcePath) +
            L" -map 0:v:0 -map 0:a:0? -c:v libvpx-vp9 -pix_fmt yuv420p -crf 31 -b:v 0"
            L" -deadline good -cpu-used 3 -row-mt 1 -tile-columns 2 -g 120 -threads " + std::to_wstring(threads) +
            L" -c:a libopus -b:a 128k -f webm -progress pipe:1 -nostats " + Quote(temporary.wstring());

        std::string ffmpegErrors;
        float reported = 0.0f;
        const int exitCode = RunProcess(
            ffmpeg, arguments,
            [&](const std::string& line) {
                // -progress reports "out_time_us=<microseconds>" (older builds call the
                // same number out_time_ms). Early reports can read N/A or dip while the
                // muxer settles, so only forward movement is passed on.
                if (!progress || duration <= 0.0)
                {
                    return;
                }

                for (const char* key : { "out_time_us=", "out_time_ms=" })
                {
                    if (line.rfind(key, 0) == 0)
                    {
                        const double seconds = std::strtod(line.c_str() + std::strlen(key), nullptr) / 1e6;
                        const float fraction = static_cast<float>(std::clamp(seconds / duration, 0.0, 0.999));
                        if (fraction > reported)
                        {
                            reported = fraction;
                            progress(fraction);
                        }

                        return;
                    }
                }
            },
            ffmpegErrors, cancel);

        if (exitCode != 0)
        {
            fs::remove(temporary, error);
            if (cancel != nullptr && cancel->load())
            {
                message = "Import cancelled.";
            }
            else if (exitCode < 0)
            {
                message = "ffmpeg could not be started.";
            }
            else
            {
                message = "ffmpeg failed: " + LastLines(ffmpegErrors, 2);
            }

            return false;
        }

        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        {
            fs::remove(temporary, error);
            message = "Could not write " + Narrow(target.filename().wstring()) + " (is it open in the player?).";
            return false;
        }

        if (progress)
        {
            progress(1.0f);
        }

        message = "Converted to " + Narrow(target.filename().wstring());
        return true;
    }
}
