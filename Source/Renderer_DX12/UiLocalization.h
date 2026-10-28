#pragma once

#include <string>
#include <vector>

// Translates the player-facing UI. Every text node RmlUi builds - authored in a document
// or set later through SetInnerRML by the game or the menus - passes through
// RmlUiSystemInterface::TranslateString, which asks Translate here. So the documents and
// the game keep writing English, and only this table knows the other languages.
//
// A language is Data/UI/Farkle/Localization/<code>.json: an object from English to the
// translation. Numbers are not part of a key: "You banked 1,250" is looked up as
// "You banked {0}", and the number goes back in with the language's digit grouping. Text
// with no entry is shown as written, so a missing line reads in English rather than not at
// all, and a status built from several sentences is translated sentence by sentence.
//
// Arabic is shaped (contextual letter forms, lam-alef) and put in visual right-to-left
// order here, as the default RmlUi font engine does neither. That is done per text node,
// so a line that wraps reads its lines in the wrong order; authored line breaks are fine.
namespace UiLocalization
{
    struct Language
    {
        const char* Code;
        // In the language itself, as the Language menu lists it.
        const char* Name;
    };

    // English first, then the languages the website offers, in the same order.
    const std::vector<Language>& Languages();

    // For a player who has not chosen yet: Steam's language for the game when launched from
    // Steam (unless it is English), else Windows' display language, else English.
    std::string DetectLanguage();

    // Loads the table for `code`; English, an unknown code or an unreadable file mean no
    // translation. Returns true when the language in effect changed.
    bool SetLanguage(const std::string& code);
    const std::string& GetLanguage();

    // RmlUi's TranslateString: false (and `output` untouched) when there is nothing to change.
    bool Translate(const std::string& input, std::string& output);
}
