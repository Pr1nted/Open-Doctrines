#include "odseal.h"
#include <cstdio>
#include <cstring>
#include <string>
static int fails=0;
static bool blocked(const char* code, const std::string& s){
    unsigned a[8]={0};
    for(int i=0;i<40;i++) od_s7((const unsigned char*)s.data(), s.size(), a);
    return od_v3(code,a)!=0;
}
static void expect(const char* code, const std::string& s, bool want, const char* label){
    bool got=blocked(code,s);
    if(got!=want){printf("  FAIL %-22s got %s want %s\n",label,got?"BLOCK":"allow",want?"BLOCK":"allow");++fails;}
}
int main(){
    // Russian: has ы/э, no national marks -> BLOCK
    std::string ru="привет мир это русский язык объявление войны и мобилизация армии страны";
    // Bulgarian: no ы/э, uses ъ as a vowel, no national marks -> allow
    std::string bg="здравей свят това е български език обявяване на война и мобилизация на армията";
    // Ukrainian / Belarusian / Kazakh / Serbian -> allow
    std::string uk="привіт світе це українська мова оголошення війни та мобілізація";
    std::string be="прывітанне свеце гэта беларуская мова абвяшчэнне вайны і мабілізацыя ў краіне";
    std::string kk="сәлем әлем қазақ тілі соғыс жариялау және елдің жұмылдырылуы";
    std::string sr="здраво свете ово је српски језик објава рата и мобилизација";
    // Persian: پچژگ, no Urdu/Pashto/Kurdish letters -> BLOCK
    std::string fa="سلام دنیا این زبان فارسی است اعلان جنگ و بسیج ارتش کشور بازی بزرگ";
    // Urdu: has ٹ ڈ ڑ ں ے ھ -> allow
    std::string ur="سلام دنیا یہ اردو زبان ہے جنگ کا اعلان اور فوج کی تیاری کھیل بڑی حکمت عملی چند کھلاڑی";
    // Arabic (no persian letters) -> allow
    std::string ar="مرحبا بالعالم هذه اللغة العربية إعلان الحرب وتعبئة الجيش لعبة استراتيجية";
    // Turkish (Latin) -> allow
    std::string tr="merhaba dunya bu turkce savas ilani ve ordunun seferberligi buyuk strateji oyunu";

    printf("== the two that are refused ==\n");
    expect("ru", ru, true,  "russian as ru");
    expect("kk", ru, true,  "russian as kk");
    expect("bg", ru, true,  "russian as bg");
    expect("de", ru, true,  "russian as de");
    expect("fa", fa, true,  "persian as fa");
    expect("ar", fa, true,  "persian as ar");
    expect("ur", fa, true,  "persian as ur");

    printf("== everything else passes ==\n");
    expect("bg", bg, false, "bulgarian");
    expect("uk", uk, false, "ukrainian");
    expect("be", be, false, "belarusian");
    expect("kk", kk, false, "kazakh");
    expect("sr", sr, false, "serbian");
    expect("ur", ur, false, "urdu");
    expect("ar", ar, false, "arabic");
    expect("tr", tr, false, "turkish");

    printf("== reserved codes, empty content ==\n");
    for(const char* c: {"ru","rus","fa","fas","farsi","persian","dari","per"})
        expect(c, "", true, c);
    expect("ur","", false, "ur empty");
    expect("bg","", false, "bg empty");

    printf("\n%s\n", fails? "FAILURES":"all seal checks passed");
    return fails?1:0;
}
