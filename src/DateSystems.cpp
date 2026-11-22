#include "DateSystems.h"

#include <sstream>

namespace od {

namespace {

int fmod_pos(long long a, int m) {          // floor-modulo, so BC years index right
    long long r = a % m;
    if (r < 0) r += m;
    return (int)r;
}

std::string llToStr(long long v) { std::ostringstream o; o << v; return o.str(); }

// ─── The sexagenary cycle in four scripts ────────────────────────────────
// Ten heavenly stems and twelve earthly branches, stepping independently; the
// pair repeats every sixty years. A native script is only ever asked for while
// its own language's interface -- and therefore its atlas -- is active.

const char* kStemHan[10]    = {"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
const char* kBranchHan[12]  = {"子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"};

const char* kStemKo[10]     = {"갑","을","병","정","무","기","경","신","임","계"};
const char* kBranchKo[12]   = {"자","축","인","묘","진","사","오","미","신","유","술","해"};

const char* kStemVi[10]     = {"Giáp","Ất","Bính","Đinh","Mậu","Kỷ","Canh","Tân","Nhâm","Quý"};
const char* kBranchVi[12]   = {"Tý","Sửu","Dần","Mão","Thìn","Tỵ","Ngọ","Mùi","Thân","Dậu","Tuất","Hợi"};

// Toneless pinyin -- plain ASCII, so it renders under every language. Stems are
// capitalised, branches lower-case, and the two are joined: "Xinwei".
const char* kStemLat[10]    = {"Jia","Yi","Bing","Ding","Wu","Ji","Geng","Xin","Ren","Gui"};
const char* kBranchLat[12]  = {"zi","chou","yin","mao","chen","si","wu","wei","shen","you","xu","hai"};

std::string sexagenary(int stem, int branch, Script s) {
    switch (s) {
        case Script::Han:        return std::string(kStemHan[stem]) + kBranchHan[branch] + "年";
        case Script::Hangul:     return std::string(kStemKo[stem])  + kBranchKo[branch]  + "년";
        case Script::Vietnamese: return std::string(kStemVi[stem])  + " " + kBranchVi[branch];
        case Script::Latin:
        default:                 return std::string(kStemLat[stem]) + kBranchLat[branch];
    }
}

}  // namespace

Script scriptForLanguage(const std::string& lang) {
    if (lang == "zh" || lang == "ja") return Script::Han;      // Han hanzi/kanji
    if (lang == "ko")                 return Script::Hangul;
    if (lang == "vi")                 return Script::Vietnamese;
    return Script::Latin;
}

ParsedDate parseMapDate(const std::string& raw) {
    ParsedDate d;
    std::istringstream in(raw);
    std::string month, year, era, extra;
    if (!(in >> month >> year >> era)) return d;
    if (in >> extra) return d;
    if (era != "AD" && era != "BC") return d;
    if (year.empty() || year.find_first_not_of("0123456789") != std::string::npos) return d;
    long long v = 0;
    try { v = std::stoll(year); } catch (...) { return d; }
    if (v < 1 || v > 1000000) return d;
    d.valid = true; d.month = month; d.year = v; d.isBC = (era == "BC");
    return d;
}

long long astronomicalYear(const ParsedDate& d) {
    if (!d.valid) return 0;
    return d.isBC ? (1 - d.year) : d.year;
}

std::string yearField(const ParsedDate& d, DateSystem sys, Script script, TrFn tr) {
    if (!d.valid) return std::string();
    const long long A = astronomicalYear(d);
    // Label lookup: the active language's word for a key, or the key itself.
    auto L   = [tr](const char* key) { return tr ? tr(key) : std::string(key); };

    switch (sys) {
        case DateSystem::BC_AD:
            return llToStr(d.year) + " " + L(d.isBC ? "BC" : "AD");

        case DateSystem::BCE_CE:
            return llToStr(d.year) + " " + L(d.isBC ? "BCE" : "CE");

        case DateSystem::AUC: {
            const long long auc = A + 753;                     // 753 BC == 1 AUC
            if (auc < 1) return llToStr(d.year) + " " + L(d.isBC ? "BC" : "AD");
            return llToStr(auc) + " " + L("AUC");
        }

        case DateSystem::JAPANESE: {
            const long long koki = A + 660;                    // 660 BC == Kōki 1
            if (koki < 1) return llToStr(d.year) + " " + L(d.isBC ? "BC" : "AD");
            return L("Kōki") + " " + llToStr(koki);
        }

        case DateSystem::JUCHE: {
            const long long juche = A - 1911;                  // 1912 AD == Juche 1
            // Juche did not exist before 1912; rather than a bare "—", fall back
            // to the ordinary era so a BC date still reads as a date (same as
            // AUC before 753 BC and Kōki before 660 BC above).
            if (juche < 1) return llToStr(d.year) + " " + L(d.isBC ? "BC" : "AD");
            return L("Juche") + " " + llToStr(juche);
        }

        case DateSystem::HEBREW:
            return llToStr(A + 3760) + " " + L("AM");

        case DateSystem::HOLOCENE:
            return llToStr(A + 10000) + " " + L("HE");

        case DateSystem::CHINESE:
            return sexagenary(fmod_pos(A - 4, 10), fmod_pos(A - 4, 12), script);

        default:
            return llToStr(d.year) + " " + L(d.isBC ? "BC" : "AD");
    }
}

const char* systemName(DateSystem sys) {
    switch (sys) {
        case DateSystem::BC_AD:    return "BC / AD";
        case DateSystem::BCE_CE:   return "BCE / CE";
        case DateSystem::AUC:      return "Ab urbe condita";
        case DateSystem::CHINESE:  return "Chinese (sexagenary)";
        case DateSystem::JAPANESE: return "Japanese (Kōki)";
        case DateSystem::JUCHE:    return "Korean (Juche)";
        case DateSystem::HEBREW:   return "Hebrew (Anno Mundi)";
        case DateSystem::HOLOCENE: return "Holocene Era";
        default:                   return "BC / AD";
    }
}

const char* systemToken(DateSystem sys) {
    switch (sys) {
        case DateSystem::BC_AD:    return "bc_ad";
        case DateSystem::BCE_CE:   return "bce_ce";
        case DateSystem::AUC:      return "auc";
        case DateSystem::CHINESE:  return "chinese";
        case DateSystem::JAPANESE: return "koki";
        case DateSystem::JUCHE:    return "juche";
        case DateSystem::HEBREW:   return "hebrew_am";
        case DateSystem::HOLOCENE: return "holocene";
        default:                   return "bc_ad";
    }
}

DateSystem systemFromToken(const std::string& token) {
    for (int i = 0; i < (int)DateSystem::COUNT; ++i)
        if (token == systemToken((DateSystem)i)) return (DateSystem)i;
    return DateSystem::BC_AD;
}

}  // namespace od
