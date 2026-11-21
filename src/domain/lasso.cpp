#include "domain/lasso.h"

namespace bl {

void Lasso::bounds(double* x0, double* y0, double* x1, double* y1) const {
    if (!closed()) {
        *x0 = *y0 = *x1 = *y1 = 0.0;
        return;
    }
    double min_x = points[0].x;
    double max_x = points[0].x;
    double min_y = points[0].y;
    double max_y = points[0].y;
    for (const Point& p : points) {
        if (p.x < min_x) min_x = p.x;
        if (p.x > max_x) max_x = p.x;
        if (p.y < min_y) min_y = p.y;
        if (p.y > max_y) max_y = p.y;
    }
    *x0 = min_x;
    *y0 = min_y;
    *x1 = max_x;
    *y1 = max_y;
}

bool Lasso::key_equals(const Lasso& other) const {
    if (points.size() != other.points.size()) return false;
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].x != other.points[i].x) return false;
        if (points[i].y != other.points[i].y) return false;
    }
    return true;
}

}  // namespace bl
