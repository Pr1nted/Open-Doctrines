#include "UiScale.h"

namespace odUi {
namespace {
float s_scale = 1.0f;
bool  s_inTexture = false;
bool  s_inFrame = false;
Matrix s_screenProjection = {};
}
float scale() { return s_scale; }
bool inTexture() { return s_inTexture; }
void setInTexture(bool on) { s_inTexture = on; }
bool inFrame() { return s_inFrame; }
void setInFrame(bool on) { s_inFrame = on; }
Matrix& screenProjection() { return s_screenProjection; }
void setScale(float s) { s_scale = (s > 0.1f && s < 8.0f) ? s : 1.0f; }
}  // namespace odUi
