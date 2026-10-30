// Stage 4: the flow algorithm (include/flow_ref.hpp, mirrored by shaders/flow_*.comp) against known motion. No GPU needed.
#include "flow_ref.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
using namespace flowref;

static float Hash(int x, int y, int seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
    return float(h & 0xffffff) / float(0x1000000);
}
// a texture with detail at several scales, defined on the whole plane so that shifting it needs no wrap-around
static float Texture(float x, float y, int seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < 4; ++o) {
        const float s = float(4 << o);                    // cell size 4, 8, 16, 32 pixels
        const float fx = x / s, fy = y / s;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float tx = fx - float(ix), ty = fy - float(iy);
        const float a = Hash(ix, iy, seed + o), b = Hash(ix + 1, iy, seed + o), c = Hash(ix, iy + 1, seed + o), d = Hash(ix + 1, iy + 1, seed + o);
        const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
        sum += amp * ((a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy);
        norm += amp; amp *= 0.6f;
    }
    return sum / norm;
}

// scene: texture moved by (dx, dy) full-resolution pixels between A and B
static void Scene(int w, int h, float dx, float dy, Img& a, Img& b) {
    a = Img(w, h); b = Img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) { a.at(x, y) = Texture(float(x), float(y), 7); b.at(x, y) = Texture(float(x) - dx, float(y) - dy, 7); }
}

struct Err { double mean; double frac_bad; };
// error against the expected vector (given in level-1 pixels) over a rectangle of the level-1 field
static Err Measure(const Flow& f, float ex, float ey, int x0, int y0, int x1, int y1) {
    double sum = 0; int bad = 0, n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const size_t i = f.idx(x, y);
            const double e = std::hypot(f.x[i] - ex, f.y[i] - ey);
            sum += e; bad += e > 1.5; ++n;
        }
    return {sum / n, double(bad) / n};
}

int main() {
    const int W = 320, H = 240;
    // 1) global motion of several sizes (full-resolution pixels): the field must match in the interior
    struct Case { float dx, dy; } cases[] = {{0, 0}, {6, 0}, {-10, 4}, {3, -5}, {24, -14}, {-30, 2}, {60, 30}, {-64, 28}};
    for (auto c : cases) {
        Img a, b; Scene(W, H, c.dx, c.dy, a, b);
        Pyramid pa = BuildPyramid(a), pb = BuildPyramid(b);
        Flow f = Estimate(pa, pb);
        const int m = 40;                                     // margin (level-1 pixels) so that content entering the frame is not judged
        Err e = Measure(f, c.dx / 2, c.dy / 2, m, m, f.w - m, f.h - m);
        std::printf("global (%g,%g): mean error %.3f px (level 1), %.1f%% above 1.5 px\n", c.dx, c.dy, e.mean, 100 * e.frac_bad);
        const bool half = (int(c.dx) & 1) || (int(c.dy) & 1);            // odd full-resolution shifts are half a pixel at level 1: integer vectors cannot be exact
        CHECK(half ? (e.mean < 0.9 && e.frac_bad < 0.08) : (e.mean < 0.3 && e.frac_bad < 0.02));
    }
    // 2) a moving object in front of a static background; a vector is judged inside the object and in the far background
    {
        const int w = 320, h = 240; Img a(w, h), b(w, h);
        const float odx = 16, ody = -8;                       // object motion
        auto in_obj = [&](float x, float y, float ox, float oy) { return x >= 110 + ox && x < 210 + ox && y >= 80 + oy && y < 160 + oy; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                a.at(x, y) = in_obj(float(x), float(y), 0, 0) ? Texture(float(x), float(y), 21) : Texture(float(x), float(y), 3);
                b.at(x, y) = in_obj(float(x), float(y), odx, ody) ? Texture(float(x) - odx, float(y) - ody, 21) : Texture(float(x), float(y), 3);
            }
        Flow fa = Estimate(BuildPyramid(a), BuildPyramid(b)), fb = Estimate(BuildPyramid(b), BuildPyramid(a));
        Err eo = Measure(fa, odx / 2, ody / 2, 62, 46, 98, 74);          // inside the object (level 1: x 55..105, y 40..80)
        Err eb = Measure(fa, 0, 0, 5, 5, 40, 30);                         // static background in a corner
        std::printf("object: inside mean %.3f (%.1f%% bad), background mean %.3f (%.1f%% bad)\n", eo.mean, 100 * eo.frac_bad, eb.mean, 100 * eb.frac_bad);
        CHECK(eo.mean < 0.8 && eo.frac_bad < 0.1);
        CHECK(eb.mean < 0.5 && eb.frac_bad < 0.05);
        // 2b) a moving object on a FLAT background (the vkcube case): the flat background must come out as zero flow, not as noise
    {
        const int w = 320, h = 240; Img a(w, h), b(w, h);
        const float odx = 6, ody = 2;
        auto in_obj = [&](float x, float y, float ox, float oy) { return x >= 110 + ox && x < 210 + ox && y >= 80 + oy && y < 160 + oy; };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                a.at(x, y) = in_obj(float(x), float(y), 0, 0) ? Texture(float(x), float(y), 21) : 0.07f;
                b.at(x, y) = in_obj(float(x), float(y), odx, ody) ? Texture(float(x) - odx, float(y) - ody, 21) : 0.07f;
            }
        Flow f = Estimate(BuildPyramid(a), BuildPyramid(b));
        // level-1 rectangles: the corners of the frame, far from the object (object spans x 55..105, y 40..80 at level 1)
        Err tl = Measure(f, 0, 0, 0, 0, 40, 25), br = Measure(f, 0, 0, f.w - 40, f.h - 25, f.w, f.h);
        std::printf("flat background: corners mean %.3f / %.3f px, bad %.1f%% / %.1f%%\n", tl.mean, br.mean, 100 * tl.frac_bad, 100 * br.frac_bad);
        CHECK(tl.mean < 0.2 && br.mean < 0.2 && tl.frac_bad < 0.02 && br.frac_bad < 0.02);    // without the zero candidate: ~20% of the corner is wrong
    }
    // 3) forward/backward consistency: low inside the object, high where the object covers background
        std::vector<float> occ = Occlusion(fa, fb);
        double inside = 0, n_in = 0, behind = 0, n_beh = 0;
        for (int y = 46; y < 74; ++y) for (int x = 62; x < 98; ++x) { inside += occ[fa.idx(x, y)]; ++n_in; }
        // background of A that the object covers in B: the strip right of the object (A x 210..226 full resolution, level 1: 105..113)
        for (int y = 50; y < 70; ++y) for (int x = 106; x < 112; ++x) { behind += occ[fa.idx(x, y)]; ++n_beh; }
        std::printf("occlusion mask: inside object %.3f, covered strip %.3f\n", inside / n_in, behind / n_beh);
        CHECK(inside / n_in < 0.1);
        CHECK(behind / n_beh > inside / n_in + 0.1);
    }
    // 4) a flat image (no texture): the flow stays zero instead of wandering
    {
        Img a(W, H), b(W, H);
        for (auto& v : a.v) v = 0.5f; for (auto& v : b.v) v = 0.5f;
        Flow f = Estimate(BuildPyramid(a), BuildPyramid(b));
        Err e = Measure(f, 0, 0, 0, 0, f.w, f.h);
        CHECK(e.mean < 0.01);
    }
    // 5) odd sizes: the pyramid rounds up and nothing reads out of range (run under -fsanitize in development)
    {
        Img a, b; Scene(333, 187, 8, -6, a, b);
        Flow f = Estimate(BuildPyramid(a), BuildPyramid(b));
        CHECK(f.w == 167 && f.h == 94);
        Err e = Measure(f, 4, -3, 40, 30, f.w - 40, f.h - 30);
        CHECK(e.mean < 0.6);
    }
    // 6) the interpolation: at t = 0.5 a shifted texture is reconstructed far better than by the plain blend
    {
        const float dx = 12, dy = -6;
        Img a, b; Scene(W, H, dx, dy, a, b);
        Pyramid pa = BuildPyramid(a), pb = BuildPyramid(b);
        Flow f = Estimate(pa, pb);
        Flow g = Estimate(pb, pa);
        std::vector<float> mask = Occlusion(f, g);
        for (float t : {0.25f, 0.5f, 0.75f}) {
            double ew = 0, eb = 0; int n = 0;
            for (int y = 60; y < H - 60; ++y)
                for (int x = 60; x < W - 60; ++x) {
                    const float truth = Texture(float(x) - t * dx, float(y) - t * dy, 7);
                    ew += std::fabs(InterpPixel(a, b, f, mask, x, y, t) - truth);
                    eb += std::fabs((1 - t) * a.get(x, y) + t * b.get(x, y) - truth);
                    ++n;
                }
            std::printf("interpolation t=%.2f: mean error %.4f (blend %.4f)\n", t, ew / n, eb / n);
            CHECK(ew < 0.25 * eb);
        }
        // the same with the field stopped at level 2 (quarter resolution, VKIFRAME_FLOW_LEVEL=2): cheaper, and still far better than the blend
        {
            Flow f2 = Estimate(pa, pb, 1), g2 = Estimate(pb, pa, 1);
            std::vector<float> mask2 = Occlusion(f2, g2);
            CHECK(f2.w == (W / 2 + 1) / 2 && f2.h == (H / 2 + 1) / 2);
            for (float t : {0.25f, 0.5f, 0.75f}) {
                double ew = 0, eb = 0; int n = 0;
                for (int y = 60; y < H - 60; ++y)
                    for (int x = 60; x < W - 60; ++x) {
                        const float truth = Texture(float(x) - t * dx, float(y) - t * dy, 7);
                        ew += std::fabs(InterpPixel(a, b, f2, mask2, x, y, t, 4.0f) - truth);
                        eb += std::fabs((1 - t) * a.get(x, y) + t * b.get(x, y) - truth);
                        ++n;
                    }
                std::printf("interpolation (level 2) t=%.2f: mean error %.4f (blend %.4f)\n", t, ew / n, eb / n);
                CHECK(ew < 0.25 * eb);
            }
        }
        // t = 0 and t = 1 return the source frames (up to the tiny sub-pixel sampling at t = 1)
        double e0 = 0; int n0 = 0;
        for (int y = 40; y < H - 40; ++y) for (int x = 40; x < W - 40; ++x) { e0 += std::fabs(InterpPixel(a, b, f, mask, x, y, 0.0f) - a.get(x, y)); ++n0; }
        CHECK(e0 / n0 < 1e-4);
    }
    // 7) a fractional shift: the sub-pixel fit gives a fractional vector and a better interpolation than whole-pixel vectors (level-2 field, as the layer uses)
    {
        const float dx = 13.5f, dy = -6.5f;
        Img a, b; Scene(W, H, dx, dy, a, b);
        Pyramid pa = BuildPyramid(a), pb = BuildPyramid(b);
        double werr[2];
        for (int sub = 0; sub < 2; ++sub) {
            Flow f = Estimate(pa, pb, 1, sub != 0), g = Estimate(pb, pa, 1, sub != 0);
            std::vector<float> mask = Occlusion(f, g);
            double ew = 0; int n = 0;
            for (int y = 60; y < H - 60; ++y)
                for (int x = 60; x < W - 60; ++x) { ew += std::fabs(InterpPixel(a, b, f, mask, x, y, 0.5f, 4.0f) - Texture(float(x) - 0.5f * dx, float(y) - 0.5f * dy, 7)); ++n; }
            werr[sub] = ew / n;
            if (sub) {   // the finished field is in level-2 pixels: the expected vector is (dx, dy) / 4
                const size_t i = f.idx(f.w / 2, f.h / 2);
                std::printf("sub-pixel vector (%.3f, %.3f), expected (%.3f, %.3f)\n", f.x[i], f.y[i], dx / 4, dy / 4);
                CHECK(std::fabs(f.x[i] - dx / 4) < 0.35 && std::fabs(f.y[i] - dy / 4) < 0.35);
            }
        }
        std::printf("interpolation at a fractional shift: whole-pixel vectors %.4f, sub-pixel %.4f\n", werr[0], werr[1]);
        CHECK(werr[1] < werr[0]);
    }
    // 8) outlines: a moving object over a moving-different background; the colour-guided vectors and the cost fade must not hurt, and
    //    must cut the error in a band around the object's outline (level-2 field, as the layer uses)
    {
        const int w = 320, h = 240; Img a(w, h), b(w, h);
        const float odx = 12, ody = 0, t = 0.5f;   // motion along the top and bottom edges: no occlusion there, so only the vector field itself matters
        auto in_obj = [&](float x, float y, float ox, float oy) { return x >= 110 + ox && x < 210 + ox && y >= 80 + oy && y < 160 + oy; };
        auto truth = [&](float x, float y, float tt) { return in_obj(x, y, tt * odx, tt * ody) ? 0.7f + 0.15f * Texture(x - tt * odx, y - tt * ody, 21) : 0.25f + 0.15f * Texture(x, y, 3); };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) { a.at(x, y) = truth(float(x), float(y), 0); b.at(x, y) = truth(float(x), float(y), 1); }
        Pyramid pa = BuildPyramid(a), pb = BuildPyramid(b);
        Flow f = Estimate(pa, pb, 1, true), g = Estimate(pb, pa, 1, true);
        std::vector<float> mask = Occlusion(f, g);
        auto dist_to_outline = [&](int x, int y) {   // distance to the object's rectangle edge in A (x 110..210, y 80..160)
            const float dxe = std::min(std::fabs(float(x) - 110.0f), std::fabs(float(x) - 210.0f)), dye = std::min(std::fabs(float(y) - 80.0f), std::fabs(float(y) - 160.0f));
            const bool inx = x >= 110 && x < 210, iny = y >= 80 && y < 160;
            return (inx && iny) ? std::min(dxe, dye) : (inx ? dye : (iny ? dxe : std::hypot(dxe, dye)));
        };
        double cost_sorted_n = 0;
        struct Mode { const char* name; InterpOpts o; double band = 0, all = 0; int nb = 0, na = 0; } modes[3];
        modes[0].name = "plain bilinear"; modes[0].o.guided = false; modes[0].o.conf = false;
        modes[1].name = "guided";         modes[1].o.conf = false;
        modes[2].name = "guided + conf";
        for (int y = 20; y < h - 20; ++y)
            for (int x = 20; x < w - 20; ++x) {
                const float ref = truth(float(x), float(y), t);
                const bool band = x >= 135 && x < 185 && (std::fabs(float(y) - 80.0f) < 6.0f || std::fabs(float(y) - 160.0f) < 6.0f);
                for (auto& m : modes) {
                    const double e = std::fabs(InterpPixel(a, b, f, mask, x, y, t, 4.0f, m.o) - ref);
                    m.all += e; ++m.na; if (band) { m.band += e; ++m.nb; }
                }
            }
        (void)cost_sorted_n;
        { double mc = 0; for (float c : f.cost) mc = std::max<double>(mc, c); std::printf("max cost %.3f (field %dx%d)\n", mc, f.w, f.h); }
        for (auto& m : modes) std::printf("outline test, %-15s: error near outline %.4f, whole frame %.4f\n", m.name, m.band / m.nb, m.all / m.na);
        CHECK(modes[1].band / modes[1].nb < modes[0].band / modes[0].nb);
        CHECK(modes[2].all / modes[2].na < 1.15 * modes[0].all / modes[0].na);
    }
    // 9) occlusion strips: a sprite moving over a static background; behind and in front of it the plain interpolation cross-fades (a ghost).
    //    The candidate search must pick the right vector there and cut the error in a band around the sprite
    {
        const int w = 320, h = 240; Img a(w, h), b(w, h);
        const float odx = 12, ody = -4, t = 0.5f;
        auto in_obj = [&](float x, float y, float ox, float oy) { return x >= 110 + ox && x < 210 + ox && y >= 80 + oy && y < 160 + oy; };
        auto truth = [&](float x, float y, float tt) { return in_obj(x, y, tt * odx, tt * ody) ? 0.7f + 0.15f * Texture(x - tt * odx, y - tt * ody, 21) : 0.25f + 0.15f * Texture(x, y, 3); };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) { a.at(x, y) = truth(float(x), float(y), 0); b.at(x, y) = truth(float(x), float(y), 1); }
        Pyramid pa = BuildPyramid(a), pb = BuildPyramid(b);
        Flow f = Estimate(pa, pb, 1, true), g = Estimate(pb, pa, 1, true);
        std::vector<float> mask = Occlusion(f, g);
        InterpOpts off; off.cand = false;
        InterpOpts on;
        double e_off = 0, e_on = 0, a_off = 0, a_on = 0; int nb = 0, na = 0;
        for (int y = 30; y < h - 30; ++y)
            for (int x = 30; x < w - 30; ++x) {
                const float ref = truth(float(x), float(y), t);
                const bool band = (x >= 100 && x < 235 && y >= 70 && y < 170) && !(x >= 130 && x < 200 && y >= 100 && y < 140);
                const double eo = std::fabs(InterpPixel(a, b, f, mask, x, y, t, 4.0f, off, &g) - ref), en = std::fabs(InterpPixel(a, b, f, mask, x, y, t, 4.0f, on, &g) - ref);
                a_off += eo; a_on += en; ++na; if (band) { e_off += eo; e_on += en; ++nb; }
            }
        std::printf("occlusion strips: error around the sprite %.4f -> %.4f with the candidate search, whole frame %.4f -> %.4f\n", e_off / nb, e_on / nb, a_off / na, a_on / na);
        CHECK(e_on < 0.85 * e_off);
        CHECK(a_on < 1.05 * a_off);
    }
    std::printf("PASS: flow reference\n");
    return 0;
}
