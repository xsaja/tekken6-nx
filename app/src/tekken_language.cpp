// tekken6-nx (2026-11-20, on request): the game's language follows the console's. Tekken 6 asks the Xbox 360
// for the player's language (XGetLanguage, the user_language cvar); with user_language not set in katamari.toml, the
// Switch's system language picks it when the game has it (this release: English, Japanese, French, German, Spanish,
// Italian, Korean, Russian), English otherwise. A user_language line in katamari.toml still wins.
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DECLARE(uint32_t, user_language);  // sdk/src/kernel/xam/xam_user.cpp

namespace tekken {

namespace {
#if defined(__SWITCH__)
// Xbox 360 XGetLanguage ids: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 12 Russian
// (8 Traditional Chinese, 9 Portuguese, 10 Polish / Simplified Chinese depending on the XDK: not in this release).
uint32_t XboxLanguageFor(SetLanguage l) {
    switch (l) {
        case SetLanguage_JA: return 2;
        case SetLanguage_DE: return 3;
        case SetLanguage_FR:
        case SetLanguage_FRCA: return 4;
        case SetLanguage_ES:
        case SetLanguage_ES419: return 5;
        case SetLanguage_IT: return 6;
        case SetLanguage_KO: return 7;
        case SetLanguage_RU: return 12;
        default: return 1;  // English (US / GB), and every language the game does not have
    }
}
#endif
}  // namespace

void ApplyConsoleLanguage() {
#if defined(__SWITCH__)
    if (rex::cvar::GetFlagSource("user_language") != rex::cvar::Source::kDefault) {
        REXLOG_INFO("[language] user_language = {} from the config (the console's language is not used)",
                    REXCVAR_GET(user_language));
        return;
    }
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    Result rc = setInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = setGetSystemLanguage(&code);
        if (R_SUCCEEDED(rc)) {
            rc = setMakeLanguage(code, &lang);
        }
        setExit();
    }
    if (R_FAILED(rc)) {
        REXLOG_WARN("[language] could not read the console's language (0x{:X}): English", rc);
        return;
    }
    const uint32_t xbox = XboxLanguageFor(lang);
    rex::cvar::SetFlagByName("user_language", std::to_string(xbox));
    REXLOG_INFO("[language] console language {} -> user_language = {}", static_cast<int>(lang), xbox);
#endif
}

}  // namespace tekken
