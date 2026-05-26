#define NOMINMAX
#include "HRIRDatabase.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline int MakeKey(int elev, int azim)
{
    // Elevation range: -90..90, azimuth range: 0..355
    // Shift elevation by 90 to keep key positive: 0..180
    return (elev + 90) * 10000 + azim;
}

// ---------------------------------------------------------------------------
// ParseFilename
//
// Handles the two formats present in the MIT KEMAR dataset:
//   L0e005a.wav      (elevation  0, azimuth   5)
//   L-10e005a.wav    (elevation -10, azimuth   5)
//   R90e000a.wav     (elevation 90, azimuth   0)
// ---------------------------------------------------------------------------
bool HRIRDatabase::ParseFilename(const std::string& name, char& ear, int& elev, int& azim)
{
    if (name.size() < 8)
        return false;

    ear = name[0];
    if (ear != 'L' && ear != 'R')
        return false;

    // Find 'e' separator (after optional '-' sign and digits)
    size_t ePos = name.find('e', 1);
    if (ePos == std::string::npos)
        return false;

    // Parse elevation substring between [1, ePos)
    std::string elevStr = name.substr(1, ePos - 1);
    try { elev = std::stoi(elevStr); }
    catch (...) { return false; }

    // Parse azimuth substring between (ePos, last 'a')
    size_t aPos = name.rfind('a');
    if (aPos == std::string::npos || aPos <= ePos)
        return false;

    std::string azimStr = name.substr(ePos + 1, aPos - ePos - 1);
    try { azim = std::stoi(azimStr); }
    catch (...) { return false; }

    return true;
}

// ---------------------------------------------------------------------------
// LoadWav
//
// Reads a 16-bit PCM mono WAV file and converts samples to float [-1, +1].
// ---------------------------------------------------------------------------
bool HRIRDatabase::LoadWav(const std::filesystem::path& path,
                            std::vector<float>& outSamples,
                            std::string& outError)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.string().c_str(), "rb") != 0 || !f)
    {
        outError = "Cannot open: " + path.string();
        return false;
    }

    auto readU16 = [&](uint16_t& v) { return fread(&v, 2, 1, f) == 1; };
    auto readU32 = [&](uint32_t& v) { return fread(&v, 4, 1, f) == 1; };
    auto readTag = [&](char tag[4]) { return fread(tag, 1, 4, f) == 4; };

    char tag[4];
    uint32_t u32; uint16_t u16;

    // RIFF header
    if (!readTag(tag) || strncmp(tag, "RIFF", 4) != 0) { fclose(f); outError = "Not a RIFF file: " + path.string(); return false; }
    if (!readU32(u32)) { fclose(f); outError = "Truncated RIFF: " + path.string(); return false; }
    if (!readTag(tag) || strncmp(tag, "WAVE", 4) != 0) { fclose(f); outError = "Not a WAVE file: " + path.string(); return false; }

    uint16_t audioFmt = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, dataSize = 0;
    bool foundFmt = false, foundData = false;

    while (!foundData)
    {
        if (!readTag(tag)) break;
        uint32_t chunkSize = 0;
        if (!readU32(chunkSize)) break;

        if (strncmp(tag, "fmt ", 4) == 0)
        {
            if (!readU16(audioFmt))   { break; }
            if (!readU16(numChannels)){ break; }
            if (!readU32(sampleRate)) { break; }
            fseek(f, 6, SEEK_CUR); // byteRate + blockAlign
            if (!readU16(bitsPerSample)) { break; }
            if (chunkSize > 16)
                fseek(f, (long)(chunkSize - 16), SEEK_CUR);
            foundFmt = true;
        }
        else if (strncmp(tag, "data", 4) == 0)
        {
            dataSize = chunkSize;
            foundData = true;
        }
        else
        {
            fseek(f, (long)chunkSize, SEEK_CUR);
        }
    }

    if (!foundFmt || !foundData)
    {
        fclose(f);
        outError = "Missing fmt/data chunk: " + path.string();
        return false;
    }

    if (audioFmt != 1 || bitsPerSample != 16)
    {
        fclose(f);
        outError = "Only 16-bit PCM WAV supported: " + path.string();
        return false;
    }

    const uint32_t numSamples = dataSize / (bitsPerSample / 8) / numChannels;
    outSamples.resize(numSamples);

    constexpr float kScale = 1.0f / 32768.0f;
    for (uint32_t i = 0; i < numSamples; ++i)
    {
        int16_t s = 0;
        if (fread(&s, 2, 1, f) != 1) { outSamples[i] = 0.0f; continue; }
        // Skip extra channels (mono expected, but just in case)
        for (uint16_t c = 1; c < numChannels; ++c)
        {
            int16_t dummy = 0;
            fread(&dummy, 2, 1, f);
        }
        outSamples[i] = static_cast<float>(s) * kScale;
    }

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
bool HRIRDatabase::Load(const std::filesystem::path& rootDir, std::string& outError)
{
    m_loaded = false;
    m_entries.clear();
    m_index.clear();
    m_elevations.clear();
    m_rootDir = rootDir;

    if (!std::filesystem::exists(rootDir))
    {
        outError = "HRTF directory not found: " + rootDir.string();
        return false;
    }

    // Temporary: accumulate L entries first, match R entries later.
    // key = elev*10000+azim -> index in m_entries
    // We iterate all sub-directories that start with "elev".
    for (const auto& dir : std::filesystem::directory_iterator(rootDir))
    {
        if (!dir.is_directory()) continue;
        const std::string dirName = dir.path().filename().string();
        if (dirName.substr(0, 4) != "elev") continue;

        for (const auto& file : std::filesystem::directory_iterator(dir.path()))
        {
            if (!file.is_regular_file()) continue;
            const std::string stem = file.path().stem().string();
            const std::string ext  = file.path().extension().string();
            if (ext != ".wav" && ext != ".WAV") continue;

            char ear = 0; int elev = 0, azim = 0;
            if (!ParseFilename(stem, ear, elev, azim)) continue;

            int key = MakeKey(elev, azim);

            if (ear == 'L')
            {
                auto it = m_index.find(key);
                if (it == m_index.end())
                {
                    Entry e;
                    e.elevation = elev;
                    e.azimuth   = azim;
                    m_index[key] = static_cast<int>(m_entries.size());
                    m_entries.push_back(std::move(e));
                    it = m_index.find(key);
                }
                std::string err;
                if (!LoadWav(file.path(), m_entries[it->second].left, err))
                    outError += err + "\n";
            }
            else if (ear == 'R')
            {
                auto it = m_index.find(key);
                if (it == m_index.end())
                {
                    Entry e;
                    e.elevation = elev;
                    e.azimuth   = azim;
                    m_index[key] = static_cast<int>(m_entries.size());
                    m_entries.push_back(std::move(e));
                    it = m_index.find(key);
                }
                std::string err;
                if (!LoadWav(file.path(), m_entries[it->second].right, err))
                    outError += err + "\n";
            }
        }
    }

    if (m_entries.empty())
    {
        outError = "No HRIR entries loaded from: " + rootDir.string();
        return false;
    }

    // Build sorted elevation list
    std::map<int, bool> elevSet;
    for (const auto& e : m_entries)
        elevSet[e.elevation] = true;
    for (const auto& kv : elevSet)
        m_elevations.push_back(kv.first);

    m_loaded = true;
    return true;
}

// ---------------------------------------------------------------------------
// GetAzimuths
// ---------------------------------------------------------------------------
std::vector<int> HRIRDatabase::GetAzimuths(int elevationDeg) const
{
    std::vector<int> result;
    for (const auto& e : m_entries)
        if (e.elevation == elevationDeg)
            result.push_back(e.azimuth);
    std::sort(result.begin(), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// Find
// ---------------------------------------------------------------------------
const HRIRDatabase::Entry* HRIRDatabase::Find(int elev, int azim) const
{
    auto it = m_index.find(MakeKey(elev, azim));
    if (it == m_index.end()) return nullptr;
    return &m_entries[it->second];
}

// ---------------------------------------------------------------------------
// BracketElevation
// ---------------------------------------------------------------------------
void HRIRDatabase::BracketElevation(float elev, int& lo, int& hi, float& t) const
{
    if (m_elevations.empty()) { lo = hi = 0; t = 0.0f; return; }

    elev = std::max(static_cast<float>(m_elevations.front()),
           std::min(static_cast<float>(m_elevations.back()), elev));

    // Find first elevation >= elev
    auto it = std::lower_bound(m_elevations.begin(), m_elevations.end(),
                               static_cast<int>(std::floor(elev)));

    if (it == m_elevations.end())
    {
        lo = hi = m_elevations.back(); t = 0.0f; return;
    }
    if (it == m_elevations.begin())
    {
        lo = hi = m_elevations.front(); t = 0.0f; return;
    }

    hi = *it;
    lo = *std::prev(it);

    if (hi == lo) { t = 0.0f; return; }
    t = (elev - static_cast<float>(lo)) / static_cast<float>(hi - lo);
}

// ---------------------------------------------------------------------------
// BracketAzimuth
// ---------------------------------------------------------------------------
void HRIRDatabase::BracketAzimuth(int elev, float azim, int& lo, int& hi, float& t) const
{
    std::vector<int> az = GetAzimuths(elev);
    if (az.empty()) { lo = hi = 0; t = 0.0f; return; }

    // Wrap azimuth to [0, 360)
    while (azim < 0.0f)   azim += 360.0f;
    while (azim >= 360.0f) azim -= 360.0f;

    auto it = std::lower_bound(az.begin(), az.end(), static_cast<int>(azim));

    if (it == az.end())
    {
        // Wrap: bracket last az and first az (wrapping around 360)
        lo = az.back();
        hi = az.front();
        float span = 360.0f - static_cast<float>(lo) + static_cast<float>(hi);
        t = span > 0.0f ? (azim - static_cast<float>(lo)) / span : 0.0f;
        if (t < 0.0f) t += 1.0f;
        return;
    }
    if (it == az.begin())
    {
        lo = az.back();
        hi = az.front();
        float span = 360.0f - static_cast<float>(lo) + static_cast<float>(hi);
        t = span > 0.0f ? (azim + 360.0f - static_cast<float>(lo)) / span : 0.0f;
        return;
    }

    hi = *it;
    lo = *std::prev(it);
    if (hi == lo) { t = 0.0f; return; }
    t = (azim - static_cast<float>(lo)) / static_cast<float>(hi - lo);
}

// ---------------------------------------------------------------------------
// Blend4
// ---------------------------------------------------------------------------
void HRIRDatabase::Blend4(const Entry* e00, const Entry* e10,
                           const Entry* e01, const Entry* e11,
                           float ta, float te,
                           std::vector<float>& outLeft,
                           std::vector<float>& outRight)
{
    outLeft.assign(kHrirLength, 0.0f);
    outRight.assign(kHrirLength, 0.0f);

    // Weights for bilinear blend:
    //   e00 = loElev, loAzim   weight = (1-te)(1-ta)
    //   e10 = loElev, hiAzim   weight = (1-te)(ta)
    //   e01 = hiElev, loAzim   weight = (te)(1-ta)
    //   e11 = hiElev, hiAzim   weight = (te)(ta)
    const float w00 = (1.0f - te) * (1.0f - ta);
    const float w10 = (1.0f - te) * ta;
    const float w01 = te * (1.0f - ta);
    const float w11 = te * ta;

    auto accumulate = [&](const Entry* e, float wL, float wR)
    {
        if (!e) return;
        for (int i = 0; i < kHrirLength; ++i)
        {
            if (i < static_cast<int>(e->left.size()))
                outLeft[i]  += e->left[i]  * wL;
            if (i < static_cast<int>(e->right.size()))
                outRight[i] += e->right[i] * wR;
        }
    };

    accumulate(e00, w00, w00);
    accumulate(e10, w10, w10);
    accumulate(e01, w01, w01);
    accumulate(e11, w11, w11);
}

// ---------------------------------------------------------------------------
// GetHRIR
// ---------------------------------------------------------------------------
void HRIRDatabase::GetHRIR(float elevationDeg, float azimuthDeg,
                             std::vector<float>& outLeft,
                             std::vector<float>& outRight) const
{
    if (!m_loaded || m_entries.empty())
    {
        outLeft.assign(kHrirLength, 0.0f);
        outRight.assign(kHrirLength, 0.0f);
        return;
    }

    int loElev, hiElev;
    float te;
    BracketElevation(elevationDeg, loElev, hiElev, te);

    int loAzLo, hiAzLo;
    float taLo;
    BracketAzimuth(loElev, azimuthDeg, loAzLo, hiAzLo, taLo);

    int loAzHi, hiAzHi;
    float taHi;
    BracketAzimuth(hiElev, azimuthDeg, loAzHi, hiAzHi, taHi);

    // Use average azimuth interpolant when elevations differ
    float ta = (te < 0.5f) ? taLo : taHi;

    const Entry* e00 = Find(loElev, loAzLo);
    const Entry* e10 = Find(loElev, hiAzLo);
    const Entry* e01 = Find(hiElev, loAzHi);
    const Entry* e11 = Find(hiElev, hiAzHi);

    // Fall back: if missing entries, use whatever is available
    if (!e00 && !e10 && !e01 && !e11)
    {
        // Last resort: first entry
        const Entry& fallback = m_entries[0];
        outLeft  = fallback.left;
        outRight = fallback.right;
        outLeft.resize(kHrirLength, 0.0f);
        outRight.resize(kHrirLength, 0.0f);
        return;
    }

    Blend4(e00, e10, e01, e11, ta, te, outLeft, outRight);
}
