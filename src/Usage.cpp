#include "Usage.h"

#include "Config.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace odusage {

const char* bucketFor(double seconds) {
    // Five ranges, and deliberately coarse. A number in seconds is a much
    // sharper thing than "how long do people play for" needs, and sharper data
    // is easier to combine with something else into an identification.
    if (seconds <  60.0)   return "<1m";
    if (seconds <  300.0)  return "1-5m";
    if (seconds <  900.0)  return "5-15m";
    if (seconds < 3600.0)  return "15-60m";
    return "60m+";
}

const char* surface() {
#if defined(__EMSCRIPTEN__)
    return "web";
#elif defined(__ANDROID__)
    return "android";
#else
    return "desktop";
#endif
}

}  // namespace odusage

void odUsagePushConsent(const Config& cfg) {
#ifdef __EMSCRIPTEN__
    // Two values only, and the endpoint is built from the issuer this build
    // was compiled against rather than passed in from anywhere -- so a page
    // that was somehow tampered with cannot redirect the report.
    const std::string endpoint =
        cfg.accountIssuer.empty() ? std::string() : cfg.accountIssuer + "/usage";

    // The strings are passed as arguments and read with UTF8ToString, never
    // pasted into a script, so there is nothing here to escape.
    EM_ASM({
        if (typeof window.odUsageSetConsent === 'function') {
            window.odUsageSetConsent($0 !== 0, UTF8ToString($1), UTF8ToString($2));
        }
    }, cfg.usageReports ? 1 : 0, endpoint.c_str(), odusage::surface());
#else
    // Desktop and Android do not report yet: there is no equivalent of
    // pagehide to hang it on, and a report sent from a shutdown path that may
    // not run is a measurement that quietly under-counts. Left undone rather
    // than done badly -- the setting still saves, and turns this on the day it
    // is wired.
    (void)cfg;
#endif
}
