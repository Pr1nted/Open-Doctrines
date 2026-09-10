#include "OpenLink.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include "raylib.h"
#endif

namespace odlink {

void open(const std::string& url) {
    if (url.empty()) return;

#ifdef __EMSCRIPTEN__
    // The URL is passed as an ARGUMENT, not pasted into a script string.
    // raylib's own web OpenURL builds `window.open('<url>')` by formatting and
    // then guards it by refusing any url containing a quote -- which is a
    // denylist standing between a string and an eval. Handing the pointer over
    // and reading it with UTF8ToString means the value is never parsed as
    // JavaScript, so there is nothing to escape and nothing to refuse.
    EM_ASM({
        var u = UTF8ToString($0);
        if (typeof window.odOpenExternal === 'function') window.odOpenExternal(u);
        else window.open(u, '_blank');
    }, url.c_str());
#else
    OpenURL(url.c_str());
#endif
}

}  // namespace odlink
