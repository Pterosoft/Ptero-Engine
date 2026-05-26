#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <map>

#ifdef AUDIO_EXPORTS
#define AUDIO_API __declspec(dllexport)
#else
#define AUDIO_API __declspec(dllimport)
#endif

// ---------------------------------------------------------------------------
// HRIRDatabase
//
// Loads the MIT KEMAR HRTF dataset from a directory of WAV files.
//
// File naming convention (KEMAR dataset):
//   L{elev}e{azim}a.wav  -- left ear HRIR
//   R{elev}e{azim}a.wav  -- right ear HRIR
//
//   elev : elevation in degrees, e.g. -10, 0, 10 ... 90
//   azim : azimuth in degrees, zero-padded to 3 digits, 0 .. 355
//
// All measurements are 44100 Hz, 16-bit mono, 512 samples.
// ---------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable: 4251)
class AUDIO_API HRIRDatabase
{
public:
    static constexpr int kHrirLength = 512;    // samples per HRIR
    static constexpr int kSampleRate = 44100;  // Hz

    HRIRDatabase()  = default;
    ~HRIRDatabase() = default;

    HRIRDatabase(const HRIRDatabase&)            = delete;
    HRIRDatabase& operator=(const HRIRDatabase&) = delete;

    // Scan rootDir for KEMAR WAV files and load them all into memory.
    bool Load(const std::filesystem::path& rootDir, std::string& outError);

    bool IsLoaded()   const { return m_loaded; }
    int  EntryCount() const { return static_cast<int>(m_entries.size()); }

    // Retrieve an interpolated HRIR pair for an arbitrary direction.
    // elevationDeg : -90 .. +90  (clamped to available range)
    // azimuthDeg   :   0 .. 360  (wrapped)
    // outLeft/outRight are resized to kHrirLength.
    void GetHRIR(float elevationDeg, float azimuthDeg,
                 std::vector<float>& outLeft,
                 std::vector<float>& outRight) const;

    const std::vector<int>& GetElevations() const { return m_elevations; }
    std::vector<int>        GetAzimuths(int elevationDeg) const;
    const std::filesystem::path& GetRootDir() const { return m_rootDir; }

private:
    struct Entry
    {
        int   elevation = 0;
        int   azimuth   = 0;
        std::vector<float> left;
        std::vector<float> right;
    };

    static bool ParseFilename(const std::string& name, char& ear, int& elev, int& azim);
    static bool LoadWav(const std::filesystem::path& path,
                        std::vector<float>& outSamples, std::string& outError);

    static void Blend4(const Entry* e00, const Entry* e10,
                       const Entry* e01, const Entry* e11,
                       float ta, float te,
                       std::vector<float>& outLeft, std::vector<float>& outRight);

    const Entry* Find(int elev, int azim) const;
    void BracketElevation(float elev, int& lo, int& hi, float& t) const;
    void BracketAzimuth(int elev, float azim, int& lo, int& hi, float& t) const;

    // key = elev * 10000 + azim
    std::map<int, int> m_index;
    std::vector<Entry> m_entries;
    std::vector<int>   m_elevations;

    std::filesystem::path m_rootDir;
    bool                  m_loaded = false;
};
#pragma warning(pop)
