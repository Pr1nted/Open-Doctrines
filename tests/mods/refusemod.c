/* Fixture: a mod that refuses its own load. The host must record it as Failed
 * with the returned code, not treat a non-zero return as success. */
#include "gearbox.h"

GEARBOX_EXPORT("mod_load")
int32_t mod_load(void) { return 7; }
