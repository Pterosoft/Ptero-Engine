#include "pch.h"

#include "UiLocalization.h"

#include "System/DataFiles.h"
#include "System/PteroLog.h"
#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <windows.h>

namespace
{
    std::string gLanguage = "en";
    std::unordered_map<std::string, std::string> gTable;

    // Thousands and decimal separators. The English text always writes 1,250.5.
    struct NumberFormat
    {
        const char* Group;
        const char* Decimal;
    };

    NumberFormat FormatFor(const std::string& language)
    {
        static const char* const kNoBreakSpace = "\xC2\xA0";
        if (language == "de" || language == "it" || language == "es" || language == "pt-br" || language == "el" || language == "tr")
            return {".", ","};
        if (language == "fr" || language == "pt-pt" || language == "pl" || language == "cs" || language == "uk")
            return {kNoBreakSpace, ","};
        return {",", "."};
    }

    bool IsDigit(char c) { return c >= '0' && c <= '9'; }

    bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

    // The digit runs of `text`, and the text with each replaced by {0}, {1}, ... in order.
    // A run takes in "," and "." between digits, so 1,250.5 is one number.
    std::string Template(const std::string& text, std::vector<std::string>& numbers)
    {
        std::string result;
        for (std::size_t i = 0; i < text.size();)
        {
            if (!IsDigit(text[i]))
            {
                result += text[i++];
                continue;
            }
            std::size_t end = i;
            while (end < text.size() && (IsDigit(text[end])
                || ((text[end] == ',' || text[end] == '.') && end + 1 < text.size() && IsDigit(text[end + 1]) && end > i)))
                ++end;
            result += "{" + std::to_string(numbers.size()) + "}";
            numbers.push_back(text.substr(i, end - i));
            i = end;
        }
        return result;
    }

    std::string LocalizeNumber(const std::string& number)
    {
        const NumberFormat format = FormatFor(gLanguage);
        const std::size_t point = number.find('.');
        // Two points is a version, not a number; it is left alone.
        if (point != std::string::npos && number.find('.', point + 1) != std::string::npos)
            return number;
        const std::string whole = number.substr(0, point);
        std::string result;
        if (whole.find(',') == std::string::npos)
        {
            result = whole;
        }
        else
        {
            std::string digits;
            for (const char c : whole)
                if (IsDigit(c)) digits += c;
            for (std::size_t i = 0; i < digits.size(); ++i)
            {
                if (i > 0 && (digits.size() - i) % 3 == 0) result += format.Group;
                result += digits[i];
            }
        }
        if (point != std::string::npos)
            result += format.Decimal + number.substr(point + 1);
        return result;
    }

    // `translation` with {n} replaced by the n-th number of the English text.
    std::string Substitute(const std::string& translation, const std::vector<std::string>& numbers)
    {
        std::string result;
        for (std::size_t i = 0; i < translation.size(); ++i)
        {
            if (translation[i] == '{')
            {
                std::size_t end = i + 1;
                while (end < translation.size() && IsDigit(translation[end])) ++end;
                if (end > i + 1 && end < translation.size() && translation[end] == '}')
                {
                    const std::size_t index = std::stoul(translation.substr(i + 1, end - i - 1));
                    if (index < numbers.size())
                    {
                        result += LocalizeNumber(numbers[index]);
                        i = end;
                        continue;
                    }
                }
            }
            result += translation[i];
        }
        return result;
    }

    // Authored text arrives with its entities and line breaks as written, and keys are
    // stored the same way, so both sides are decoded and have their white space collapsed.
    std::string Normalize(const std::string& text)
    {
        static const std::pair<const char*, const char*> kEntities[] = {
            {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", "\xC2\xA0"},
        };
        std::string result;
        bool pendingSpace = false;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (IsSpace(text[i]))
            {
                pendingSpace = !result.empty();
                continue;
            }
            if (pendingSpace) result += ' ';
            pendingSpace = false;
            if (text[i] == '&')
            {
                bool decoded = false;
                for (const auto& [entity, character] : kEntities)
                {
                    if (text.compare(i, std::strlen(entity), entity) == 0)
                    {
                        result += character;
                        i += std::strlen(entity) - 1;
                        decoded = true;
                        break;
                    }
                }
                if (decoded) continue;
            }
            result += text[i];
        }
        return result;
    }

    bool Lookup(const std::string& text, std::string& output)
    {
        std::vector<std::string> numbers;
        const auto it = gTable.find(Template(text, numbers));
        if (it == gTable.end()) return false;
        output = Substitute(it->second, numbers);
        return true;
    }

    // The settings status line is several notes run together ("Settings applied. DLSS is
    // not available on this system."), so whatever is not a key as a whole is tried one
    // sentence at a time. Sentences without an entry stay in English.
    bool LookupSentences(const std::string& text, std::string& output)
    {
        std::vector<std::string> sentences;
        std::size_t start = 0;
        for (std::size_t i = 0; i + 1 < text.size(); ++i)
        {
            const bool stop = text[i] == '.' || text[i] == '!' || text[i] == '?'
                || (i >= 2 && text.compare(i - 2, 3, "\xE2\x80\xA6") == 0);
            if (stop && text[i + 1] == ' ')
            {
                sentences.push_back(text.substr(start, i + 1 - start));
                start = i + 2;
            }
        }
        if (sentences.empty()) return false;
        sentences.push_back(text.substr(start));

        const char* separator = gLanguage == "ja" || gLanguage == "zh-cn" ? "" : " ";
        bool translatedAny = false;
        output.clear();
        for (std::size_t i = 0; i < sentences.size(); ++i)
        {
            std::string sentence;
            if (Lookup(sentences[i], sentence)) translatedAny = true;
            else sentence = sentences[i];
            if (i > 0) output += separator;
            output += sentence;
        }
        return translatedAny;
    }

    std::u32string Utf8ToUtf32(const std::string& text)
    {
        std::u32string result;
        for (std::size_t i = 0; i < text.size();)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            const int length = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            char32_t codepoint = length == 1 ? c : length == 2 ? (c & 0x1F) : length == 3 ? (c & 0x0F) : (c & 0x07);
            for (int k = 1; k < length && i + k < text.size(); ++k)
                codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            result += codepoint;
            i += length;
        }
        return result;
    }

    std::string Utf32ToUtf8(const std::u32string& text)
    {
        std::string result;
        for (const char32_t c : text)
        {
            if (c < 0x80) result += static_cast<char>(c);
            else if (c < 0x800) { result += static_cast<char>(0xC0 | (c >> 6)); result += static_cast<char>(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { result += static_cast<char>(0xE0 | (c >> 12)); result += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); result += static_cast<char>(0x80 | (c & 0x3F)); }
            else { result += static_cast<char>(0xF0 | (c >> 18)); result += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); result += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); result += static_cast<char>(0x80 | (c & 0x3F)); }
        }
        return result;
    }

    // Arabic Presentation Forms-B for U+0621..U+064A: isolated, final, initial, medial.
    // A letter with no initial form joins only to the letter before it.
    struct ArabicForms
    {
        char32_t Isolated, Final, Initial, Medial;
    };

    constexpr ArabicForms kArabicForms[] = {
        {0xFE80, 0, 0, 0},                // hamza
        {0xFE81, 0xFE82, 0, 0},           // alef with madda
        {0xFE83, 0xFE84, 0, 0},           // alef with hamza above
        {0xFE85, 0xFE86, 0, 0},           // waw with hamza
        {0xFE87, 0xFE88, 0, 0},           // alef with hamza below
        {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // yeh with hamza
        {0xFE8D, 0xFE8E, 0, 0},           // alef
        {0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // beh
        {0xFE93, 0xFE94, 0, 0},           // teh marbuta
        {0xFE95, 0xFE96, 0xFE97, 0xFE98}, // teh
        {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // theh
        {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // jeem
        {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // hah
        {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // khah
        {0xFEA9, 0xFEAA, 0, 0},           // dal
        {0xFEAB, 0xFEAC, 0, 0},           // thal
        {0xFEAD, 0xFEAE, 0, 0},           // reh
        {0xFEAF, 0xFEB0, 0, 0},           // zain
        {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // seen
        {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // sheen
        {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // sad
        {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // dad
        {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // tah
        {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // zah
        {0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // ain
        {0xFECD, 0xFECE, 0xFECF, 0xFED0}, // ghain
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0x0640, 0x0640, 0x0640, 0x0640}, // tatweel
        {0xFED1, 0xFED2, 0xFED3, 0xFED4}, // feh
        {0xFED5, 0xFED6, 0xFED7, 0xFED8}, // qaf
        {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // kaf
        {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // lam
        {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // meem
        {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // noon
        {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // heh
        {0xFEED, 0xFEEE, 0, 0},           // waw
        {0xFEEF, 0xFEF0, 0, 0},           // alef maksura
        {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // yeh
    };

    const ArabicForms* FormsOf(char32_t c)
    {
        if (c < 0x0621 || c > 0x064A) return nullptr;
        const ArabicForms& forms = kArabicForms[c - 0x0621];
        return forms.Isolated != 0 ? &forms : nullptr;
    }

    // Lam followed by one of these is written as a single ligature (isolated, final).
    bool LamAlef(char32_t alef, char32_t& isolated, char32_t& final)
    {
        switch (alef)
        {
        case 0x0622: isolated = 0xFEF5; final = 0xFEF6; return true;
        case 0x0623: isolated = 0xFEF7; final = 0xFEF8; return true;
        case 0x0625: isolated = 0xFEF9; final = 0xFEFA; return true;
        case 0x0627: isolated = 0xFEFB; final = 0xFEFC; return true;
        default: return false;
        }
    }

    std::u32string ShapeArabic(const std::u32string& input)
    {
        // Vowel marks would need positioning the font engine does not do; the text reads
        // without them, so they are dropped.
        std::u32string text;
        for (const char32_t c : input)
            if (!((c >= 0x064B && c <= 0x065F) || c == 0x0670)) text += c;

        std::u32string result;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const ArabicForms* forms = FormsOf(text[i]);
            if (forms == nullptr)
            {
                result += text[i];
                continue;
            }
            const ArabicForms* previous = i > 0 ? FormsOf(text[i - 1]) : nullptr;
            const bool joinsBefore = previous != nullptr && previous->Medial != 0;
            char32_t isolated = 0, final = 0;
            if (text[i] == 0x0644 && i + 1 < text.size() && LamAlef(text[i + 1], isolated, final))
            {
                result += joinsBefore ? final : isolated;
                ++i;
                continue;
            }
            const bool joinsAfter = forms->Medial != 0 && i + 1 < text.size() && FormsOf(text[i + 1]) != nullptr
                && FormsOf(text[i + 1])->Final != 0;
            result += joinsBefore && joinsAfter ? forms->Medial : joinsBefore ? forms->Final : joinsAfter ? forms->Initial : forms->Isolated;
        }
        return result;
    }

    bool IsRightToLeft(char32_t c)
    {
        return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
    }

    bool IsLeftToRight(char32_t c)
    {
        if (c < 0x80) return std::isalnum(static_cast<int>(c)) != 0;
        if (IsRightToLeft(c) || c == 0x00D7 || c == 0x00F7 || (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F))
            return false;
        return c >= 0x00C0;
    }

    char32_t Mirror(char32_t c)
    {
        switch (c)
        {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '<': return '>'; case '>': return '<';
        case 0x00AB: return 0x00BB; case 0x00BB: return 0x00AB;
        default: return c;
        }
    }

    // A right-to-left paragraph in the left-to-right order the font engine lays out:
    // reversed, except that runs of Latin letters and digits keep their own order. White
    // space and punctuation between two such runs belong to them; anywhere else they take
    // the paragraph's direction.
    std::u32string VisualOrder(const std::u32string& text)
    {
        std::vector<bool> leftToRight(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (IsLeftToRight(text[i])) { leftToRight[i] = true; continue; }
            if (IsRightToLeft(text[i])) continue;
            std::size_t before = i, after = i + 1;
            while (before > 0 && !IsLeftToRight(text[before - 1]) && !IsRightToLeft(text[before - 1])) --before;
            while (after < text.size() && !IsLeftToRight(text[after]) && !IsRightToLeft(text[after])) ++after;
            leftToRight[i] = before > 0 && IsLeftToRight(text[before - 1]) && after < text.size() && IsLeftToRight(text[after]);
        }

        std::u32string result;
        for (std::size_t end = text.size(); end > 0;)
        {
            if (!leftToRight[end - 1])
            {
                result += Mirror(text[--end]);
                continue;
            }
            std::size_t start = end;
            while (start > 0 && leftToRight[start - 1]) --start;
            result.append(text, start, end - start);
            end = start;
        }
        return result;
    }

    std::string Lowercase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // A BCP 47 tag ("de-AT", "pt-PT", "zh-Hans-CN") as one of the supported codes, or empty.
    std::string MatchLanguageTag(const std::string& tag)
    {
        const std::string lower = Lowercase(tag);
        for (const auto& language : UiLocalization::Languages())
            if (lower == language.Code) return language.Code;
        const std::string base = lower.substr(0, lower.find_first_of("-_"));
        if (base == "pt") return lower.rfind("pt-pt", 0) == 0 ? "pt-pt" : "pt-br";
        if (base == "zh") return "zh-cn";
        for (const auto& language : UiLocalization::Languages())
            if (base == language.Code) return language.Code;
        return {};
    }

    // ISteamApps::GetCurrentGameLanguage, from the steam_api64.dll the launcher started.
    // Never loaded here: without it, or before SteamAPI_Init, there is no Steam language.
    std::string SteamLanguage()
    {
        const HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
        if (steam == nullptr) return {};
        using SteamAppsFn = void* (*)();
        using GetLanguageFn = const char* (*)(void*);
        const auto steamApps = reinterpret_cast<SteamAppsFn>(GetProcAddress(steam, "SteamAPI_SteamApps_v009"));
        const auto getLanguage = reinterpret_cast<GetLanguageFn>(GetProcAddress(steam, "SteamAPI_ISteamApps_GetCurrentGameLanguage"));
        void* apps = steamApps != nullptr ? steamApps() : nullptr;
        const char* name = apps != nullptr && getLanguage != nullptr ? getLanguage(apps) : nullptr;
        if (name == nullptr) return {};

        static const std::pair<const char*, const char*> kSteamNames[] = {
            {"english", "en"}, {"german", "de"}, {"french", "fr"}, {"spanish", "es"}, {"latam", "es"},
            {"italian", "it"}, {"brazilian", "pt-br"}, {"portuguese", "pt-pt"}, {"polish", "pl"},
            {"czech", "cs"}, {"greek", "el"}, {"turkish", "tr"}, {"ukrainian", "uk"}, {"arabic", "ar"},
            {"schinese", "zh-cn"}, {"tchinese", "zh-cn"}, {"japanese", "ja"}, {"koreana", "ko"},
        };
        for (const auto& [steamName, code] : kSteamNames)
            if (std::strcmp(name, steamName) == 0) return code;
        return {};
    }

    std::string WindowsLanguage()
    {
        ULONG count = 0, length = 0;
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length) || length == 0)
            return {};
        std::wstring names(length, L'\0');
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &length))
            return {};
        for (const wchar_t* name = names.c_str(); *name != L'\0'; name += wcslen(name) + 1)
        {
            std::string tag;
            for (const wchar_t* c = name; *c != L'\0'; ++c) tag += static_cast<char>(*c);
            if (std::string code = MatchLanguageTag(tag); !code.empty()) return code;
        }
        return {};
    }
}

namespace UiLocalization
{
    const std::vector<Language>& Languages()
    {
        static const std::vector<Language> languages = {
            {"en", "English"},
            {"de", "Deutsch"},
            {"fr", "Français"},
            {"es", "Español"},
            {"it", "Italiano"},
            {"pt-br", "Português (Brasil)"},
            {"pt-pt", "Português (Portugal)"},
            {"pl", "Polski"},
            {"cs", "Čeština"},
            {"el", "Ελληνικά"},
            {"tr", "Türkçe"},
            {"uk", "Українська"},
            {"ar", "العربية"},
            {"zh-cn", "简体中文"},
            {"ja", "日本語"},
            {"ko", "한국어"},
        };
        return languages;
    }

    std::string DetectLanguage()
    {
        // Steam answers "english" for every player until the store page lists more
        // languages, so English from Steam defers to Windows.
        if (std::string steam = SteamLanguage(); !steam.empty() && steam != "en") return steam;
        if (std::string windows = WindowsLanguage(); !windows.empty()) return windows;
        return "en";
    }

    bool SetLanguage(const std::string& code)
    {
        std::string language = "en";
        for (const auto& entry : Languages())
            if (code == entry.Code) language = entry.Code;
        if (language == gLanguage) return false;

        gLanguage = "en";
        gTable.clear();
        if (language == "en") return true;

        const std::filesystem::path path = DataFiles::FindDataDirectory() / L"UI" / L"Farkle" / L"Localization"
            / std::filesystem::path(language + ".json");
        std::string text;
        nlohmann::json table;
        if (DataFiles::ReadText(path, text))
            table = nlohmann::json::parse(text, nullptr, false);
        if (!table.is_object())
        {
            PTERO_LOG_WARNING("UI", "Translation %s could not be read; the UI stays in English.", path.string().c_str());
            return true;
        }
        for (const auto& [key, value] : table.items())
            if (value.is_string()) gTable[Normalize(key)] = value.get<std::string>();
        gLanguage = language;
        PTERO_LOG_INFO("UI", "Language: %s (%zu strings).", gLanguage.c_str(), gTable.size());
        return true;
    }

    const std::string& GetLanguage()
    {
        return gLanguage;
    }

    // Text the game marks with U+E000 ... U+E001 is a player's own: their Steam name or a chat
    // line. It is never translated, and stands in the key as {name}, so a translation can put
    // it wherever the language needs it: "{name} sat down at your table." Each marked span is
    // cut out here (entities decoded, as keys are) and put back by InsertNames.
    static std::string ExtractNames(const std::string& text, std::vector<std::string>& names)
    {
        static const std::string kOpen = "\xEE\x80\x80", kClose = "\xEE\x80\x81";
        std::string result;
        std::size_t at = 0;
        for (std::size_t open; (open = text.find(kOpen, at)) != std::string::npos;)
        {
            const std::size_t close = text.find(kClose, open + kOpen.size());
            if (close == std::string::npos) break;
            result += text.substr(at, open - at) + "{name}";
            names.push_back(Normalize(text.substr(open + kOpen.size(), close - open - kOpen.size())));
            at = close + kClose.size();
        }
        return result + text.substr(at);
    }

    static std::string InsertNames(const std::string& text, const std::vector<std::string>& names)
    {
        std::string result;
        std::size_t at = 0, next = 0;
        while (next < names.size())
        {
            // Right-to-left text comes back in visual order, braces mirrored.
            std::size_t found = text.find("{name}", at), mirrored = text.find("}name{", at);
            if (mirrored < found) found = mirrored;
            if (found == std::string::npos) break;
            result += text.substr(at, found - at) + names[next++];
            at = found + 6;
        }
        return result + text.substr(at);
    }

    static bool TranslateText(const std::string& input, std::string& output);

    bool Translate(const std::string& input, std::string& output)
    {
        std::vector<std::string> names;
        const std::string unmarked = ExtractNames(input, names);
        if (names.empty()) return TranslateText(input, output);
        // Always handled: even untranslated, the markers must not reach the screen.
        if (!TranslateText(unmarked, output)) output = unmarked;
        output = InsertNames(output, names);
        return true;
    }

    static bool TranslateText(const std::string& input, std::string& output)
    {
        // Unshaped Arabic letters, U+0621..U+064A. Shaped text is all presentation forms, so
        // text RmlUi hands back again (a select copying its option, say) is not reversed twice.
        const auto hasArabic = [](const std::string& text)
        {
            for (std::size_t i = 0; i + 1 < text.size(); ++i)
            {
                const unsigned char lead = static_cast<unsigned char>(text[i]), next = static_cast<unsigned char>(text[i + 1]);
                if ((lead == 0xD8 && next >= 0xA1 && next <= 0xBA) || (lead == 0xD9 && next >= 0x81 && next <= 0x8A))
                    return true;
            }
            return false;
        };
        if (gTable.empty() && !hasArabic(input)) return false;
        const std::size_t first = input.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return false;
        const std::size_t last = input.find_last_not_of(" \t\r\n");

        const std::string text = Normalize(input.substr(first, last - first + 1));
        std::string translated;
        if (gTable.empty() || (!Lookup(text, translated) && !LookupSentences(text, translated)))
        {
            // Untranslated Arabic, such as "العربية" in the Language menu, is still shaped.
            if (!hasArabic(text)) return false;
            translated = text;
        }
        if (hasArabic(translated)) translated = Utf32ToUtf8(VisualOrder(ShapeArabic(Utf8ToUtf32(translated))));

        // The white space around the text is the document's, not the sentence's.
        output = input.substr(0, first) + translated + input.substr(last + 1);
        return true;
    }
}
