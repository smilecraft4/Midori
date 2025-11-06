#include "midori/canvas.h"

#include <cassert>

#include "midori/app.h"

Canvas::Canvas(App* app) : app_(app) { assert(app); }