// ---------------------------------------------------------------------------
// The Console panel.
//
// Shows the same stream PteroLog is writing to <repo>/Logs, filtered, plus a
// command line wired to the cvar registry. The panel is a view, never a store:
// every line it draws came from the log, and every command it runs goes back
// through the log, so what is on screen and what is on disk cannot disagree.
//
// The rendered document is rebuilt only when something changed - a new line, a
// different filter - because the widget behind TextView has to re-parse the
// whole thing when it does.
// ---------------------------------------------------------------------------

#include "pch.h"
#include "Editor.h"

#include "System/CVar.h"
#include "System/PteroLog.h"
#include "../QtUi/QtUi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace
{
    // The file keeps everything; this is only how much is worth having in a
    // scrollable widget at once.
    constexpr std::size_t kMaxDisplayedLines = 800;

    // Level colours, indexed by PteroLog::Level. Warnings and errors have to
    // stand out at a glance against a wall of Info.
    const char* const kLevelColors[] = {
        "#7d8590",   // Trace
        "#9aa7b3",   // Debug
        "#d6dde5",   // Info
        "#e2b341",   // Warning
        "#f2685f",   // Error
        "#ff4d4d"    // Fatal
    };

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    void AppendEscaped(std::string& out, const std::string& text)
    {
        for (const char c : text)
        {
            switch (c)
            {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case ' ':  out += "&nbsp;"; break;
            default:   out += c;        break;
            }
        }
    }
}

void Editor::DrawConsolePanel()
{
    if (!QtUi::Begin("Console", &mShowConsolePanel))
    {
        QtUi::End();
        return;
    }

    // ---------------------------------------------------------------- filters
    static const char* const kLevelNames[] = { "Trace", "Debug", "Info", "Warning", "Error", "Fatal" };
    QtUi::SetNextItemWidth(110.0f);
    if (QtUi::Combo("Level", &mConsoleMinimumLevel, kLevelNames, 6))
        mConsoleDocumentDirty = true;

    // The category list grows as subsystems log for the first time, so it is
    // rebuilt from the log rather than hard-coded.
    const std::vector<std::string> categories = PteroLog::Categories();
    if (categories.size() != mConsoleCategoryCount)
    {
        mConsoleCategoryCount = categories.size();
        mConsoleDocumentDirty = true;
    }
    std::vector<const char*> categoryItems;
    categoryItems.reserve(categories.size() + 1);
    categoryItems.push_back("All categories");
    for (const std::string& category : categories)
        categoryItems.push_back(category.c_str());
    if (mConsoleCategoryIndex >= static_cast<int>(categoryItems.size()))
        mConsoleCategoryIndex = 0;

    QtUi::SameLine();
    QtUi::SetNextItemWidth(160.0f);
    if (QtUi::Combo("##consolecategory", &mConsoleCategoryIndex,
                    categoryItems.data(), static_cast<int>(categoryItems.size())))
        mConsoleDocumentDirty = true;

    QtUi::SetNextItemWidth(220.0f);
    if (QtUi::InputText("Filter", mConsoleFilter, sizeof(mConsoleFilter)))
        mConsoleDocumentDirty = true;

    QtUi::SameLine();
    if (QtUi::Button("Clear"))
    {
        PteroLog::Clear();
        mConsoleDocumentDirty = true;
    }
    QtUi::SameLine();
    QtUi::Checkbox("Follow", &mConsoleAutoScroll);

    // --------------------------------------------------------------- document
    const std::uint64_t total = PteroLog::TotalCount();
    const std::uint64_t evicted = PteroLog::EvictedCount();
    if (total != mConsoleLastTotal || evicted != mConsoleLastEvicted)
    {
        mConsoleLastTotal = total;
        mConsoleLastEvicted = evicted;
        mConsoleDocumentDirty = true;
    }

    if (mConsoleDocumentDirty)
    {
        mConsoleDocumentDirty = false;

        std::vector<PteroLog::Entry> entries;
        PteroLog::Snapshot(entries, kMaxDisplayedLines);

        const auto minimumLevel = static_cast<PteroLog::Level>(mConsoleMinimumLevel);
        const std::string needle = ToLower(mConsoleFilter);
        const std::string* wantedCategory =
            (mConsoleCategoryIndex > 0 && mConsoleCategoryIndex - 1 < static_cast<int>(categories.size()))
                ? &categories[static_cast<std::size_t>(mConsoleCategoryIndex - 1)]
                : nullptr;

        std::string document;
        document.reserve(entries.size() * 96);
        document += "<body style=\"white-space:pre;\">";

        std::size_t shown = 0;
        for (const PteroLog::Entry& entry : entries)
        {
            if (entry.MessageLevel < minimumLevel) continue;
            if (wantedCategory != nullptr && entry.Category != *wantedCategory) continue;
            if (!needle.empty()
                && ToLower(entry.Message).find(needle) == std::string::npos
                && ToLower(entry.Category).find(needle) == std::string::npos)
                continue;

            const int levelIndex = static_cast<int>(entry.MessageLevel);
            const char* color = (levelIndex >= 0 && levelIndex < 6) ? kLevelColors[levelIndex] : kLevelColors[2];

            char prefix[64] = {};
            std::snprintf(prefix, sizeof(prefix), "%8.2f  %-5s  %-12s  ",
                          entry.TimeSeconds, PteroLog::LevelName(entry.MessageLevel), entry.Category.c_str());

            document += "<span style=\"color:";
            document += color;
            document += ";\">";
            AppendEscaped(document, prefix);
            AppendEscaped(document, entry.Message);
            document += "</span><br>";
            ++shown;
        }

        if (shown == 0)
            document += "<span style=\"color:#7d8590;\">(nothing matches the current filter)</span>";

        document += "</body>";
        mConsoleDocument = std::move(document);
    }

    QtUi::TextView("##consoleoutput", mConsoleDocument.c_str(), UiVec2{ 0.0f, 260.0f }, mConsoleAutoScroll);

    // ----------------------------------------------------------- command line
    // The type-ahead list is built once. Every string it points at - cvar names
    // and descriptions, and the built-in command table - lives for the process,
    // so the entries stay valid without copying, and the list only has to be
    // rebuilt if a cvar is ever registered late.
    if (mConsoleCompletions.size() != CVar::Count() + CVar::Commands().size())
    {
        mConsoleCompletions.clear();
        for (const CVar::Command& builtin : CVar::Commands())
            mConsoleCompletions.push_back({ builtin.Name, builtin.Help });
        for (const CVar::Var* var : CVar::Search(std::string()))
            mConsoleCompletions.push_back({ var->Name.c_str(), var->Description.c_str() });
    }

    if (QtUi::InputTextSubmit("##consoleinput", mConsoleInput, sizeof(mConsoleInput),
                              mConsoleCompletions.data(), static_cast<int>(mConsoleCompletions.size())))
    {
        if (mConsoleInput[0] != '\0')
        {
            CVar::Execute(mConsoleInput);
            mConsoleInput[0] = '\0';
            mConsoleDocumentDirty = true;
        }
    }

    QtUi::SameLine();
    if (QtUi::Button("Help"))
        CVar::Execute("help");

    QtUi::TextDisabled("%llu lines, %llu cvars. Type to search, Enter to run, 'help' for commands.",
                       static_cast<unsigned long long>(total),
                       static_cast<unsigned long long>(CVar::Count()));

    const std::string logPath = PteroLog::SessionFilePathUtf8();
    if (!logPath.empty())
        QtUi::TextDisabled("%s", logPath.c_str());

    QtUi::End();
}
