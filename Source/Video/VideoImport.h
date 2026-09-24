#pragma once

#include "VideoAPI.h"

#include <atomic>
#include <functional>
#include <string>

// Brings a video into the project as a .webm (VP9 video, Opus audio).
//
// A .webm that already holds VP8/VP9 video is copied untouched, so importing a file that
// was prepared elsewhere never costs a generation of quality. Anything else is converted
// with the ffmpeg that ships in Binaries\ffmpeg.
namespace VideoImport
{
    // Extensions ffmpeg is asked to convert, plus .webm.
    VIDEO_API bool IsVideoFile(const std::wstring& path);
    // "*.webm;*.mp4;..." for an open-file dialog filter.
    VIDEO_API const char* GetFileDialogPattern();

    // Empty when ffmpeg cannot be found.
    VIDEO_API std::wstring FindFfmpeg();

    // Imports `sourcePath` into `targetDirectory` as <stem>.webm, replacing an existing
    // file of that name. `progress` (optional) receives 0..1 and may be called from the
    // calling thread only. Setting `cancel` (optional) aborts a conversion.
    VIDEO_API bool ImportIntoDirectory(const std::wstring& sourcePath,
                                       const std::wstring& targetDirectory,
                                       std::wstring& outputPath,
                                       std::string& message,
                                       const std::function<void(float)>& progress = {},
                                       const std::atomic<bool>* cancel = nullptr);
}
