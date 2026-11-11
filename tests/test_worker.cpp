// Linked into every test program: formulas are drawn by the app run as a
// worker (see MathRenderer.h), and a test program is not the app.
#include "markdown/MathRenderer.h"

namespace {

const bool named = (vn::math::setWorkerProgram(QString::fromUtf8(VN_APP_PATH)), true);

} // namespace
