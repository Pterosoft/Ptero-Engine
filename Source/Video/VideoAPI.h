#pragma once

// Video.dll: WebM (VP8/VP9) playback for the engine and the editor.
//
//   VideoPlayer        - decodes a .webm file on a worker thread and hands out BGRA frames
//   VideoTexture       - streams a VideoPlayer's frames into a D3D12 texture
//   VideoImport        - converts any video ffmpeg understands into a .webm under Data
//   VideoPlayerWindow  - the editor's stand-alone preview window
//
// Only the video stream is played. The importer keeps the audio track (Opus) so nothing
// is lost, but no Opus decoder ships with the engine yet.

#ifdef VIDEO_EXPORTS
#define VIDEO_API __declspec(dllexport)
#else
#define VIDEO_API __declspec(dllimport)
#endif
