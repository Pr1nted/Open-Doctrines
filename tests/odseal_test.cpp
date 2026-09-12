#include "odseal.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define od_mkdir(p) _mkdir(p)
#  define od_rmdir(p) _rmdir(p)
#else
#  include <unistd.h>
#  define od_mkdir(p) mkdir(p, 0755)
#  define od_rmdir(p) rmdir(p)
#endif
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

    // ── od_t4: the rule tables folded to one word ──
    //
    // Its whole job is to be equal on two installs of the same release and
    // unequal when the tables have been edited, so those are the two things
    // asserted -- against files this test writes, not against data/, because
    // this runs on five platforms and must not depend on the checkout's shape.
    printf("== rule-table seal ==\n");
    {
        const char* dir = "odseal_t4_tmp";
        od_mkdir(dir);
        auto put = [&](const char* name, const char* body){
            std::string path = std::string(dir) + "/" + name;
            FILE* f = fopen(path.c_str(), "wb");
            if (f) { fwrite(body, 1, strlen(body), f); fclose(f); }
        };
        auto check = [&](bool ok, const char* label){
            if (!ok) { printf("  FAIL %-22s\n", label); ++fails; }
        };
        const std::string root = std::string(dir) + "/";

        put("policies.json", "{\"policies\":[{\"id\":\"land_reform\"}]}");
        put("district_laws.json", "{\"laws\":[{\"id\":\"curfew\"}]}");

        const unsigned long long a = od_t4(root.c_str());
        check(a != 0ull, "a populated directory folds to something");
        check(od_t4(root.c_str()) == a, "the same tables fold the same twice");

        // The thing it exists to catch: a number changed in a shipped table.
        put("policies.json", "{\"policies\":[{\"id\":\"land_reform\",\"x\":9}]}");
        const unsigned long long b = od_t4(root.c_str());
        check(b != a, "an edited table folds differently");

        // A file that is not there is a difference too, not a silent zero.
        std::string gone = std::string(dir) + "/district_laws.json";
        remove(gone.c_str());
        const unsigned long long c = od_t4(root.c_str());
        check(c != 0ull && c != b, "a missing table folds differently");

        check(od_t4(nullptr) == 0ull, "no directory folds to zero");

        std::string p1 = std::string(dir) + "/policies.json";
        remove(p1.c_str());
        od_rmdir(dir);
    }

    printf("\n%s\n", fails? "FAILURES":"all seal checks passed");
    return fails?1:0;
}
