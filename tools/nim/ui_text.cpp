#include "ui_text.hpp"

#include <atomic>
#include <iterator>

namespace nim::ui {
namespace {

std::atomic<uint8_t> g_lang{uint8_t(Lang::Ja)};

struct Row {
    const char* text[size_t(Lang::Count)];
};

#define NIM_UI_ROW(key, ja, en, ko) Row{{ja, en, ko}},
constexpr Row kTable[] = {NIM_UI_STRINGS(NIM_UI_ROW)};
#undef NIM_UI_ROW

// The 13 content languages, in TextLanguage order.
constexpr S kContentLanguageKeys[] = {
    S::LangEnglish,       S::LangJapanese,          S::LangGerman,      S::LangFrench,
    S::LangSpanish,       S::LangSpanishLatam,      S::LangItalian,     S::LangKorean,
    S::LangTraditionalChinese, S::LangSimplifiedChinese, S::LangPortugueseBrazil, S::LangPolish,
    S::LangRussian,
};

} // namespace

static_assert(std::size(kTable) == size_t(S::Count), "every NIM_UI_STRINGS row needs one entry");

Lang lang() { return Lang(g_lang.load(std::memory_order_relaxed)); }

void set_lang(Lang l) {
    if (l < Lang::Count) g_lang.store(uint8_t(l), std::memory_order_relaxed);
}

const char* tr_in(S s, Lang l) {
    const auto i = size_t(s);
    if (i >= std::size(kTable) || size_t(l) >= size_t(Lang::Count)) return "";
    const char* t = kTable[i].text[size_t(l)];
    // A missing translation falls back to Japanese (the source language), never to an empty label.
    return t && *t ? t : kTable[i].text[size_t(Lang::Ja)];
}

const char* tr(S s) { return tr_in(s, lang()); }

const char* native_name(Lang l) {
    switch (l) {
    case Lang::Ja: return "日本語";
    case Lang::En: return "English";
    case Lang::Ko: return "한국어";
    default: return "?";
    }
}

const char* code(Lang l) {
    switch (l) {
    case Lang::Ja: return "ja";
    case Lang::En: return "en";
    case Lang::Ko: return "ko";
    default: return "ja";
    }
}

bool parse_lang(std::string_view c, Lang& out) {
    for (uint8_t i = 0; i < uint8_t(Lang::Count); ++i)
        if (c == code(Lang(i))) {
            out = Lang(i);
            return true;
        }
    return false;
}

const char* content_language_name(size_t index) {
    if (index >= std::size(kContentLanguageKeys)) return "?";
    return tr(kContentLanguageKeys[index]);
}

} // namespace nim::ui
