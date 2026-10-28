#include "System/Guid.h"

#include <cctype>
#include <mutex>
#include <random>

Guid Guid::New()
{
    // One generator for the process, seeded from the OS. Guarded because sequences may
    // be created on a loader thread while the editor creates tracks on the main one.
    static std::mutex mutex;
    static std::mt19937_64 generator{ std::random_device{}() ^
                                      (static_cast<std::uint64_t>(std::random_device{}()) << 32) };

    Guid guid;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        guid.High = generator();
        guid.Low = generator();
    }

    // Version 4 (random) and the RFC 4122 variant, so the text form is a well-formed UUID
    // that any external tool will accept.
    guid.High = (guid.High & ~0x000000000000f000ull) | 0x0000000000004000ull;
    guid.Low = (guid.Low & ~0xc000000000000000ull) | 0x8000000000000000ull;
    return guid;
}

std::string Guid::ToString() const
{
    static const char* kHex = "0123456789abcdef";
    std::string text;
    text.reserve(36);

    auto appendHex = [&](std::uint64_t value, int firstNibble, int count) {
        for (int i = 0; i < count; ++i)
        {
            const int shift = (15 - (firstNibble + i)) * 4;
            text.push_back(kHex[(value >> shift) & 0xf]);
        }
    };

    appendHex(High, 0, 8);
    text.push_back('-');
    appendHex(High, 8, 4);
    text.push_back('-');
    appendHex(High, 12, 4);
    text.push_back('-');
    appendHex(Low, 0, 4);
    text.push_back('-');
    appendHex(Low, 4, 12);
    return text;
}

Guid Guid::Parse(const std::string& text, bool* ok)
{
    if (ok != nullptr)
        *ok = false;

    std::size_t begin = 0;
    std::size_t end = text.size();
    if (end >= 2 && text[0] == '{' && text[end - 1] == '}')
    {
        ++begin;
        --end;
    }

    if (end - begin != 36)
        return {};

    std::uint64_t halves[2] = {};
    int nibble = 0;
    for (std::size_t i = begin; i < end; ++i)
    {
        const std::size_t position = i - begin;
        const char c = text[i];
        if (position == 8 || position == 13 || position == 18 || position == 23)
        {
            if (c != '-')
                return {};
            continue;
        }

        int value = 0;
        if (c >= '0' && c <= '9')
            value = c - '0';
        else if (c >= 'a' && c <= 'f')
            value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            value = c - 'A' + 10;
        else
            return {};

        std::uint64_t& half = halves[nibble / 16];
        half = (half << 4) | static_cast<std::uint64_t>(value);
        ++nibble;
    }

    if (ok != nullptr)
        *ok = true;

    Guid guid;
    guid.High = halves[0];
    guid.Low = halves[1];
    return guid;
}
