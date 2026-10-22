/*
 * High frame rates (ps2host/high_fps.h): the rate, the steps, the pictures
 * between them. First made for Burnout 3 (games/burnout3/docs/
 * HIGH_FPS_PLAN.md: what was measured), then made the engine's.
 *
 * The game steps at its own rate (60 a second) and stays so. Above 60 the
 * vertical blanks come more often (SetFastForward's whole multiples, exact;
 * SetBlankRate's others: 144, 165) and the game draws a picture at each;
 * each blank moves its time on by 59.94 over the rate (a half at 120), a
 * step each time that takes it past the next one - 0, 1 or more a picture
 * (HighFpsSteps, the count the game's own frame runs). The picture's place
 * between its last two steps comes from what ran: the game's own count of
 * steps, less the time the pictures moved on, kept between 0 and 1 (where
 * the counting began sets no phase).
 *
 * After each frame that stepped, every transform (an RwMatrix by shape, in
 * the game's regions) and the cameras' values are taken down; for each
 * picture each one that moved is placed between where the step before and
 * the last left it, at the picture's time; at the next frame's start it is
 * put back - where the picture left it as placed (what the draw wrote anew
 * stays). So the pictures show the game evenly, less than a step late. One
 * that jumped further than `cut` in a step is not blended; a camera's own
 * cut (a jump, a reversal, a sharp turn against the step before) neither.
 */

#include "ps2host/high_fps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps2host/run.h"
#include "ps2host/settings.h"
#include "ps2rt/generated.h"

namespace ps2host {
namespace {

HighFpsGame g_game;
bool g_configured = false;

// The pictures a second while above 60 (0: not), and the steps one blank
// moves the game's time on.
double g_rate = 0.0;
double g_per_picture = 1.0;
// The steps asked for: each blank adds g_per_picture; past 1 the game's next
// frame steps, and 1 is taken off (HighFpsSteps).
double g_time = 1.0;
// How far the picture is behind the last step the game ran (see above), and
// the picture's place between its last two steps: 1 less that.
double g_behind = 0.0;
float g_picture_time = 1.0f;
bool g_counted = false;                 // g_step_count holds the game's (since the rate changed)
u32 g_step_count = 0;
u32 g_frame_steps = 1;                  // the steps this frame of the game's ran
// Blanks since the game's frame last ran (HighFpsFrameSteps): past `gone`,
// it has stopped - a loading screen, a movie - and the rate is its own again.
// (HighFpsPicture, where the rate is chosen, runs only in its frames.)
std::atomic<int> g_blanks_since{0};
unsigned long long g_frames = 0;        // the game's frames (HighFpsFrameSteps)

struct Matrix {
    float m[16];
};
std::unordered_map<u32, Matrix> g_last, g_now;      // the transforms after the last two steps
// What the in-between placing covered: as the step left it, and as placed.
struct Placed {
    u32 address;
    Matrix was, placed;
};
std::vector<Placed> g_placed;

struct CameraValues {
    std::vector<float> last, now, placed, was;
    bool placing = false;               // `placed` is in RAM (PutBack)
    float step = 0.0f, turn = 0.0f;     // the last step's
    float way[3] = {};                  // the last step's move
    bool cut = false;                   // the last step a cut
    bool flipped = false;               // the last step only turned its way of looking round
};
std::vector<CameraValues> g_cameras;

constexpr double kStepRate = 1e6 / 16683.0;    // the blanks' own, 59.94 (threads.cpp's VblankTimer)

// What this machine kept up with in this run of the game's frames (0: no
// limit found): a rate whose pictures were often shown late (LatePictures)
// steps down a multiple of 60. Looked at every two seconds or so of the
// game's frames, the first such stretch after a change let go (its catching
// up).
double g_cap = 0.0;
int g_sampled = 0;                      // frames since the last look
int g_looks = 0;                        // looks since the rate changed
u64 g_late_seen = 0;                    // LatePictures at the last look

/* FRAME RATE, as pictures a second (60: none above the game's own):
   PS2HOST_FRAME_RATE, else the setting - 120, or "display", the screen's
   refresh (up to 240; 60 where it is not known or below 90). */
/* The game's own picture rate: 60 for a game that steps each blank, 30 for
   one that steps every second one. */
double Own() { return 60.0 * g_game.steps_per_blank; }

double Wanted() {
    static const char* env = std::getenv("PS2HOST_FRAME_RATE");
    const std::string rate = env ? std::string(env) : Setting("frame_rate", g_game.default_rate);
    const double hz = rate == "display" ? double(WindowRefreshRate()) : std::atof(rate.c_str());
    double want = hz >= 1.5 * Own() ? std::min(hz, 240.0) : Own();
    // PS2HOST_HIGH_FPS_FROM=<frame>: 60 until then (a scripted run's test:
    // its presses land as at 60, and the same screen at 60 and above is
    // compared at the same times).
    static const unsigned long long from = [] {
        const char* e = std::getenv("PS2HOST_HIGH_FPS_FROM");
        return e ? std::strtoull(e, nullptr, 10) : 0ull;
    }();
    if (FrameNumber() < from) want = Own();
    return g_cap > 0.0 ? std::min(want, g_cap) : want;
}

/* Each frame of the game's above 60: every two seconds or so, the pictures
   shown late since; more than 5% and the rate steps down (to the multiple
   of 60 below it). */
void KeepUp() {
    const int sample = int(2.0 * g_rate);
    if (++g_sampled < sample) return;
    const u64 late_now = LatePictures(), late = late_now - g_late_seen;
    g_late_seen = late_now;
    g_sampled = 0;
    if (g_looks++ == 0) return;         // (the change's own catching up)
    if (late * 20 > u64(sample)) {
        g_cap = std::max(Own(), 60.0 * std::ceil(g_rate / 60.0 - 1.0 - 1e-6));
        std::fprintf(stderr,
                     "[high fps] %llu of %d pictures late at %.0f a second: %.0f until the game's frames next stop\n",
                     static_cast<unsigned long long>(late), sample, g_rate, g_cap);
    }
}

/* The blanks at `hz`: a whole multiple of 60 (within a hertz for each 60:
   120, 180, 240) as SetFastForward's multiple, exact; another (144, 165) at
   its own rate. The steps a blank moves the game's time on. */
double Pace(double hz) {
    const int multiple = std::max(int(hz / 60.0 + 0.5), 1);
    if (std::fabs(hz - 60.0 * multiple) < multiple) {
        SetBlankRate(0.0);
        SetFastForward(unsigned(multiple));
        return g_game.steps_per_blank / multiple;
    }
    SetFastForward(1);
    SetBlankRate(hz);
    return g_game.steps_per_blank * kStepRate / hz;
}

/* The game running: its frames (HighFpsFrameSteps) went on within the last
   few HighFpsPicture calls. */
bool Running() {
    static unsigned long long seen = 0;
    static int still = 99;
    still = g_frames != seen ? 0 : still + 1;
    seen = g_frames;
    return still <= g_game.running;
}

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* An RwMatrix: its right, up and at rows of length 1 and square to each
   other, its position finite. */
bool Shaped(const float* m) {
    constexpr float kTolerance = 0.01f;
    for (int row = 0; row < 3; ++row)
        if (!(std::fabs(Dot3(m + 4 * row, m + 4 * row) - 1.0f) < kTolerance)) return false;
    if (!(std::fabs(Dot3(m, m + 4)) < kTolerance && std::fabs(Dot3(m, m + 8)) < kTolerance &&
          std::fabs(Dot3(m + 4, m + 8)) < kTolerance))
        return false;
    for (int k = 12; k < 15; ++k)
        if (!(std::fabs(m[k]) < 1e6f)) return false;
    return true;
}

/* A camera's own cut, step to step: a jump - its position moved more than
   3 times as far as in the step before (and over a unit), or over a unit
   against the way it moved in the step before, or the way it looks turned
   more than 60 degrees and twice as far as in the step before (the game cut
   to another camera, or turned it round). Such a step is shown as it is,
   not blended: a picture between the two showed neither, a one-picture
   lurch. But a way of looking only turned round (its values negated: Burnout
   3's at speed, for a step, now and then) while the camera moved on as
   before is no cut - blended as ever, those values as the step left them
   (blended they would pass through 0); taken for one, the camera stood
   still for the step's pictures, then went on: a jitter. */
void CameraTakenDown(const HighFpsCamera& camera, CameraValues& v) {
    const float* a = v.last.data() + camera.position;
    const float* b = v.now.data() + camera.position;
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float step = std::sqrt(Dot3(d, d));
    float turn = 0.0f;
    if (camera.direction >= 0) {
        const float* la = v.last.data() + camera.direction;
        const float* na = v.now.data() + camera.direction;
        // (only where the two are of a length: Burnout 3's +0x20 values
        // shrink to a few hundredths now and then, and their turns are noise)
        const float lengths = std::sqrt(Dot3(la, la) * Dot3(na, na));
        turn = lengths > 0.01f ? std::acos(std::min(1.0f, std::max(-1.0f, Dot3(la, na) / lengths))) * 57.29578f
                               : 0.0f;
    }
    const bool reversed = step > 1.0f && Dot3(d, v.way) < 0.0f;
    const bool jumped = step > 1.0f && step > 3.0f * v.step;
    v.flipped = turn > 170.0f && !jumped && !reversed;
    v.cut = jumped || reversed || (turn > 60.0f && turn > 2.0f * v.turn && !v.flipped);
    std::memcpy(v.way, d, sizeof v.way);
    v.step = step;
    if (!v.flipped) v.turn = turn;
}

/* The transforms and the cameras' values as the step left them. */
void TakeDown() {
    for (size_t k = 0; k < g_game.cameras.size(); ++k) {
        const HighFpsCamera& camera = g_game.cameras[k];
        CameraValues& v = g_cameras[k];
        const size_t bytes = v.now.size() * sizeof(float);
        if (const u8* p = generated::RamPointer(camera.address, u32(bytes))) {
            std::memcpy(v.last.data(), v.now.data(), bytes);
            std::memcpy(v.now.data(), p, bytes);
            CameraTakenDown(camera, v);
        }
    }
    g_last.swap(g_now);
    g_now.clear();
    for (const HighFpsRegion& region : g_game.transforms) {
        const u8* base = generated::RamPointer(region.begin, region.end - region.begin);
        if (!base) continue;
        for (u32 at = 0; at + 64 <= region.end - region.begin; at += 16) {
            // The right row's length first, in place: most of 2 MB a step is
            // no matrix, and the copy of each candidate was most of the scan.
            float right[3];
            std::memcpy(right, base + at, sizeof right);
            if (!(std::fabs(Dot3(right, right) - 1.0f) < 0.01f)) continue;
            Matrix m;
            std::memcpy(m.m, base + at, sizeof m.m);
            if (!Shaped(m.m)) continue;
            g_now.emplace(region.begin + at, m);
            at += 48;                   // (its rows are not another's)
        }
    }
}

void Normalise(float* v) {
    const float length = std::sqrt(Dot3(v, v));
    if (length > 0.0f)
        for (int k = 0; k < 3; ++k) v[k] /= length;
}

/* A transform `t` of the way from a to b: the position between, the
   rotation the rows' blend made orthonormal again (at, then up square to
   it, then right from the two, its handedness the original's). */
Matrix Between(const Matrix& a, const Matrix& b, float t) {
    Matrix out = b;
    float right[3], up[3], at[3];
    const float s = 1.0f - t;
    for (int k = 0; k < 3; ++k) {
        right[k] = s * a.m[k] + t * b.m[k];
        up[k] = s * a.m[4 + k] + t * b.m[4 + k];
        at[k] = s * a.m[8 + k] + t * b.m[8 + k];
        out.m[12 + k] = a.m[12 + k] + t * (b.m[12 + k] - a.m[12 + k]);
    }
    Normalise(at);
    const float d = Dot3(up, at);
    for (int k = 0; k < 3; ++k) up[k] -= d * at[k];
    Normalise(up);
    float cross[3] = {up[1] * at[2] - up[2] * at[1], up[2] * at[0] - up[0] * at[2], up[0] * at[1] - up[1] * at[0]};
    if (Dot3(cross, right) < 0.0f)
        for (float& x : cross) x = -x;
    for (int k = 0; k < 3; ++k) {
        out.m[k] = cross[k];
        out.m[4 + k] = up[k];
        out.m[8 + k] = at[k];
    }
    return out;
}

/* A transform that moved, placed `t` of the way from the step before to
   the last. */
void Place(u32 address, const Matrix& last, const Matrix& now, float t) {
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = now.m[12 + k] - last.m[12 + k];
    const float moved = std::sqrt(Dot3(d, d));
    bool turned = false;
    for (int k = 0; k < 12 && !turned; ++k) turned = now.m[k] != last.m[k];
    if ((moved == 0.0f && !turned) || moved > g_game.cut) return;
    u8* p = generated::RamPointer(address, 64);
    if (!p) return;
    Placed placed{address, {}, {}};
    std::memcpy(placed.was.m, p, sizeof placed.was.m);
    placed.placed = placed.was;
    const Matrix between = Between(last, now, t);
    std::memcpy(placed.placed.m, between.m, 12 * 4);     // the rows (their fourth words as they were)
    std::memcpy(placed.placed.m + 12, between.m + 12, 3 * 4);
    std::memcpy(p, placed.placed.m, sizeof placed.placed.m);
    g_placed.push_back(placed);
}

bool AValue(float v) { return std::fabs(v) > 1e-6f && std::fabs(v) < 1e6f; }

/* For the picture drawn now: the cameras' values and each transform that
   moved, `t` of the way from the step before to the last (1: as the last
   left them). */
void PlaceBetween(float t) {
    if (t >= 1.0f || g_last.empty()) return;
    for (size_t k = 0; k < g_game.cameras.size(); ++k) {
        CameraValues& v = g_cameras[k];
        const size_t count = v.now.size();
        float* camera = reinterpret_cast<float*>(generated::RamPointer(g_game.cameras[k].address,
                                                                       u32(count * sizeof(float))));
        if (!camera || v.cut) continue;
        std::memcpy(v.was.data(), camera, count * sizeof(float));
        std::memcpy(v.placed.data(), camera, count * sizeof(float));
        const int way = v.flipped ? g_game.cameras[k].direction : -1;
        for (size_t i = 0; i < count; ++i) {
            const float a = v.last[i], b = v.now[i];
            if (a == b || !AValue(a) || !AValue(b) || !(std::fabs(b - a) < g_game.cut)) continue;
            if (way >= 0 && int(i) >= way && int(i) < way + 3) continue;    // (turned round: the step's)
            v.placed[i] = a + t * (b - a);
        }
        std::memcpy(camera, v.placed.data(), count * sizeof(float));
        v.placing = true;
    }
    for (const auto& [address, now] : g_now) {
        const auto last = g_last.find(address);
        if (last != g_last.end()) Place(address, last->second, now, t);
    }
}

/* What the placing covered, back as the step left it - where the picture
   left it as placed (what the draw wrote anew stays). */
void PutBack() {
    for (const Placed& placed : g_placed) {
        u8* p = generated::RamPointer(placed.address, 64);
        if (p && std::memcmp(p, placed.placed.m, sizeof placed.placed.m) == 0)
            std::memcpy(p, placed.was.m, sizeof placed.was.m);
    }
    g_placed.clear();
    for (size_t k = 0; k < g_game.cameras.size(); ++k) {
        CameraValues& v = g_cameras[k];
        if (!v.placing) continue;
        const size_t bytes = v.now.size() * sizeof(float);
        u8* camera = generated::RamPointer(g_game.cameras[k].address, u32(bytes));
        if (camera && std::memcmp(camera, v.placed.data(), bytes) == 0) std::memcpy(camera, v.was.data(), bytes);
        v.placing = false;
    }
}

/* PS2HOST_HIGH_FPS_TRACE=<file>: a line (CSV) for each frame of the game's
   above 60, as its picture is placed - its time, the steps it ran, where
   it is between them, how many transforms were placed, and each camera's
   position as the last two steps left it and as placed, how far it went in
   the step and how far its way of looking turned (the dot of the two
   steps' directions). engine/tools/high_fps_trace.py finds where a
   camera's path jumps. */
std::FILE* TraceFile() {
    static std::FILE* file = [] {
        const char* path = std::getenv("PS2HOST_HIGH_FPS_TRACE");
        std::FILE* f = path ? std::fopen(path, "w") : nullptr;
        if (f) {
            std::fprintf(f, "frame,ms,rate,ticks,t,placed");
            for (size_t k = 0; k < g_game.cameras.size(); ++k)
                std::fprintf(f, ",c%zu_last_x,c%zu_last_y,c%zu_last_z,c%zu_now_x,c%zu_now_y,c%zu_now_z,"
                             "c%zu_x,c%zu_y,c%zu_z,c%zu_step,c%zu_turn", k, k, k, k, k, k, k, k, k, k, k);
            std::fprintf(f, "\n");
        }
        return f;
    }();
    return file;
}

void Trace(float t) {
    std::FILE* f = TraceFile();
    static unsigned long long traced = 0;
    if (!f || traced == g_frames) return;
    traced = g_frames;
    std::fprintf(f, "%llu,%.3f,%.0f,%u,%.4f,%zu", g_frames, double(ClockNs()) / 1e6, g_rate, g_frame_steps,
                 double(t), g_placed.size());
    for (size_t k = 0; k < g_game.cameras.size(); ++k) {
        const HighFpsCamera& camera = g_game.cameras[k];
        const CameraValues& v = g_cameras[k];
        const float* a = v.last.data() + camera.position;
        const float* b = v.now.data() + camera.position;
        const float* placed = (v.placing ? v.placed.data() : v.now.data()) + camera.position;
        const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float dot = camera.direction >= 0 ? Dot3(v.last.data() + camera.direction, v.now.data() + camera.direction)
                                                : 1.0f;
        std::fprintf(f, ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.5f", a[0], a[1], a[2], b[0], b[1], b[2],
                     placed[0], placed[1], placed[2], std::sqrt(Dot3(d, d)), dot);
    }
    std::fprintf(f, "\n");
    if (g_frames % 120 == 0) std::fflush(f);
}

/* PS2HOST_HIGH_FPS_POKE=<hex address>: the float there raised by 0.5 after
   each picture's placing, before its draw - a picture that changes says the
   draw reads it (a camera's input, not a value the draw makes anew). */
void Poke() {
    static const u32 address = [] {
        const char* e = std::getenv("PS2HOST_HIGH_FPS_POKE");
        return e ? u32(std::strtoul(e, nullptr, 16)) : 0u;
    }();
    if (!address) return;
    if (u8* p = generated::RamPointer(address, 4)) {
        float v;
        std::memcpy(&v, p, 4);
        v += 0.5f;
        std::memcpy(p, &v, 4);
    }
}

bool Blend() {
    static const bool blend = std::getenv("PS2HOST_HIGH_FPS_NO_BLEND") == nullptr;   // to compare
    return blend;
}

/* Back to the game's own rate, the placing forgotten. */
void Reset() {
    g_last.clear();
    g_now.clear();
    g_time = 1.0;
    g_behind = 0.0;
    g_counted = false;
}

}  // namespace

void HighFpsConfigure(const HighFpsGame& game) {
    g_game = game;
    g_configured = true;
    g_cameras.assign(game.cameras.size(), CameraValues{});
    for (size_t k = 0; k < game.cameras.size(); ++k) {
        CameraValues& v = g_cameras[k];
        const size_t count = size_t(std::max(game.cameras[k].floats, 1));
        v.last.assign(count, 0.0f);
        v.now.assign(count, 0.0f);
        v.placed.assign(count, 0.0f);
        v.was.assign(count, 0.0f);
    }
}

void HighFpsFrameStart() { PutBack(); }

bool HighFpsFrameSteps(u32 steps_so_far, unsigned blanks) {
    g_blanks_since = 0;
    ++g_frames;
    if (g_rate > 0.0) KeepUp();
    g_frame_steps = g_counted ? steps_so_far - g_step_count : 1;
    g_step_count = steps_so_far;
    g_counted = true;
    if (g_rate > 0.0) {
        g_behind += double(g_frame_steps) - g_per_picture * std::max(1u, blanks);
        if (g_behind >= 1.0) g_behind -= std::floor(g_behind);
        if (g_behind < 0.0) g_behind = 0.0;
        g_picture_time = float(1.0 - g_behind);
    } else {
        g_picture_time = 1.0f;
    }
    return !(g_rate > 0.0 && g_frame_steps == 0);
}

void HighFpsStepped() {
    if (g_rate > 0.0) TakeDown();
}

void HighFpsPicture() {
    // Down to 60 only once the game has not run for a moment (`settle`
    // calls): a change drains the GS thread - a hitch.
    static int differing = 0;
    const bool running = Running();
    if (!running) g_cap = 0.0;          // (the next run tries the setting's rate again)
    const double want = running ? Wanted() : Own();
    const double now = g_rate > 0.0 ? g_rate : Own();
    differing = std::fabs(want - now) >= 0.5 ? differing + 1 : 0;
    if (differing >= (want > Own() ? 1 : g_game.settle)) {
        differing = 0;
        std::fprintf(stderr, "[high fps] %.0f pictures a second\n", want);
        PutBack();
        Reset();
        g_sampled = 0;
        g_looks = 0;
        if (want > Own()) {
            g_rate = want;
            g_per_picture = Pace(want);
        } else {
            g_rate = 0.0;
            g_per_picture = 1.0;
            SetBlankRate(0.0);
            SetFastForward(1);
        }
    }
    if (g_rate > 0.0 && Blend()) {
        PutBack();
        PlaceBetween(g_picture_time);
        Poke();
        Trace(g_picture_time);
    }
}

int HighFpsSteps(unsigned blanks, int most) {
    if (g_rate <= 0.0) return -1;
    g_time += g_per_picture * std::max(1u, blanks);
    int steps = 0;
    while (g_time > 1.0 + 1e-6) {
        g_time -= 1.0;
        ++steps;
    }
    if (steps > most) {
        steps = most;
        g_time = 1.0;
    }
    return steps;
}

void HighFpsBlank() {
    if (g_rate <= 0.0 || ++g_blanks_since <= g_game.gone * std::max(1, int(g_rate / 60.0 + 0.5))) return;
    HighFpsLeave("the game's frames stopped");
}

void HighFpsLeave(const char* why) {
    if (g_rate <= 0.0) return;
    std::fprintf(stderr, "[high fps] %.0f pictures a second (%s)\n", Own(), why);
    g_cap = 0.0;
    g_placed.clear();
    for (CameraValues& v : g_cameras) v.placing = false;
    g_rate = 0.0;
    g_per_picture = 1.0;
    Reset();
    SetBlankRate(0.0);
    SetFastForward(1);
}

double HighFpsRate() { return g_rate; }

float HighFpsPictureTime() { return g_picture_time; }

double HighFpsOwnRate() { return g_configured ? Own() : 0.0; }

}  // namespace ps2host
