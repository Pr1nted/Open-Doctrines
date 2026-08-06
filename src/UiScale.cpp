#include "UiScale.h"

namespace odUi {
namespace {
float s_scale = 1.0f;
}
float scale() { return s_scale; }
void setScale(float s) { s_scale = (s > 0.1f && s < 8.0f) ? s : 1.0f; }
}  // namespace odUi
