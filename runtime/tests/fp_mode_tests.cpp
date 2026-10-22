/*
 * The VUs round toward zero and take denormals as zero; the EE's FPU rounds
 * toward zero in software; the host's own float code rounds to nearest. These
 * tests pin each against results worked out from the IEEE-754 definitions -
 * operands chosen so that rounding to nearest and toward zero differ - so a
 * platform whose float mode is set wrongly, or not at all, fails here rather
 * than in a game's pictures. (Two paths compared with each other, as the VU
 * tests do, would agree even then.)
 */

#include "fp_mode_tests.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "ps2rt/fp_mode.h"
#include "ps2rt/generated.h"
#include "ps2rt/lifted.h"

namespace ps2rt_tests {

namespace {

float F(u32 bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

u32 B(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

// At run time, in the mode then set: volatile operands in functions not
// inlined, so nothing is folded at compile time (which rounds to nearest).
PS2RT_NOINLINE u32 Add(u32 a, u32 b) { volatile float x = F(a), y = F(b); return B(x + y); }
PS2RT_NOINLINE u32 Sub(u32 a, u32 b) { volatile float x = F(a), y = F(b); return B(x - y); }
PS2RT_NOINLINE u32 Mul(u32 a, u32 b) { volatile float x = F(a), y = F(b); return B(x * y); }
PS2RT_NOINLINE u32 Div(u32 a, u32 b) { volatile float x = F(a), y = F(b); return B(x / y); }

// Operands whose exact results lie between two floats, nearer the one away
// from zero: to nearest takes it, toward zero the other.
constexpr u32 kOne = 0x3F800000u, kMinusOne = 0xBF800000u, kTiny = 0x33C00000u;    // 1.5 * 2^-24
constexpr u32 kOneAndHalf = 0x3FC00000u, kOneAndHalfUp = 0x3FC00001u, kThree = 0x40400000u;
constexpr u32 kDenormal = 0x00400000u, kSmallestNormal = 0x00800000u, kHalf = 0x3F000000u;

void HostDefaultMode(Check check, const char* when) {
    char what[160];
    auto expect = [&](bool ok, const char* claim) {
        std::snprintf(what, sizeof what, "%s: %s", when, claim);
        check(ok, what);
    };
    expect(Add(kOne, kTiny) == 0x3F800001u, "add rounds to nearest");
    expect(Sub(kMinusOne, kTiny) == 0xBF800001u, "a negative result rounds to nearest");
    expect(Mul(kOneAndHalf, kOneAndHalfUp) == 0x40100001u, "mul rounds to nearest");
    expect(Div(kOne, kThree) == 0x3EAAAAABu, "div rounds to nearest");
    expect(Add(kDenormal, 0) == kDenormal, "a denormal operand is kept");
    expect(Mul(kSmallestNormal, kHalf) == kDenormal, "a denormal result is kept");
}

void HostVuMode(Check check, const char* when) {
    char what[160];
    auto expect = [&](bool ok, const char* claim) {
        std::snprintf(what, sizeof what, "%s: %s", when, claim);
        check(ok, what);
    };
    expect(Add(kOne, kTiny) == kOne, "add rounds toward zero");
    expect(Sub(kMinusOne, kTiny) == kMinusOne, "a negative result rounds toward zero");
    expect(Mul(kOneAndHalf, kOneAndHalfUp) == 0x40100000u, "mul rounds toward zero");
    expect(Div(kOne, kThree) == 0x3EAAAAAAu, "div rounds toward zero");
    expect(Add(kDenormal, 0) == 0, "a denormal operand is zero");
    expect(Mul(kSmallestNormal, kHalf) == 0, "a denormal result is zero");
}

void HostFloatMode(Check check) {
    HostDefaultMode(check, "default float mode");
    const ps2rt::FloatControl before = ps2rt::ReadFloatControl();
    {
        ps2rt::VuFloatMode vu;
        HostVuMode(check, "VU float mode");
        {
            ps2rt::VuFloatMode nested;
            HostVuMode(check, "VU float mode, nested");
        }
        HostVuMode(check, "VU float mode, after a nested scope");
    }
    check(ps2rt::ReadFloatControl() == before, "VU float mode: the control register restored exactly");
    HostDefaultMode(check, "default float mode, after a VU scope");
    {
        ps2rt::VuFloatMode unset(false);
        HostDefaultMode(check, "VU float mode not set");
    }
}

u32 Cop2Word(u32 funct, u32 fd, u32 fs, u32 ft) {
    return 0x4A000000u | (0xFu << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;   // .xyzw
}

void MacroMode(Check check) {
    // To nearest only when verification asks for it (PS2RT_EE_ROUND_NEAREST),
    // as the interpreter it is compared with rounds.
    const bool nearest = lifted::kMacroNearest;
    for (int inline_form = 0; inline_form < 2; ++inline_form) {
        auto c = std::make_unique<lifted::Context>();
        const u32 a[4] = {kOne, kMinusOne, kOneAndHalf, kDenormal};
        const u32 b[4] = {kTiny, 0xB3C00000u, kOneAndHalfUp, 0};
        std::memcpy(c->vf[1], a, sizeof a);
        std::memcpy(c->vf[2], b, sizeof b);
        auto run = [&](u32 word) { inline_form ? lifted::Cop2(*c, word) : lifted::Cop2Slow(*c, word); };
        run(Cop2Word(0x28, 3, 1, 2));                                   // VADD.xyzw vf3, vf1, vf2
        run(Cop2Word(0x2A, 4, 1, 2));                                   // VMUL.xyzw vf4, vf1, vf2
        const char* form = inline_form ? "macro mode (Cop2)" : "macro mode (Cop2Slow)";
        char what[160];
        auto expect = [&](bool ok, const char* claim) {
            std::snprintf(what, sizeof what, "%s, %s: %s", form, nearest ? "to nearest" : "as the VU", claim);
            check(ok, what);
        };
        expect(c->vf[3][0] == (nearest ? 0x3F800001u : kOne), "VADD rounds");
        expect(c->vf[3][1] == (nearest ? 0xBF800001u : kMinusOne), "VADD rounds a negative result");
        expect(c->vf[3][3] == 0, "VADD takes a denormal as zero");
        expect(c->vf[4][2] == (nearest ? 0x40100001u : 0x40100000u), "VMUL rounds");
        HostDefaultMode(check, "default float mode, after macro mode");
    }
}

void EeFpu(Check check) {
    // As the console, the EE's FPU rounds toward zero in software (eefpu.cpp),
    // whatever the host's mode: in the default mode and inside a VU scope
    // alike. To nearest - verification's PS2RT_EE_ROUND_NEAREST - it takes the
    // thread's own rounding, which is to nearest wherever EE code runs (never
    // inside a VU scope), so only the default mode is checked then.
    const bool nearest = std::getenv("PS2RT_EE_ROUND_NEAREST") != nullptr;
    for (int in_vu_mode = 0; in_vu_mode < (nearest ? 1 : 2); ++in_vu_mode) {
        ps2rt::VuFloatMode mode(in_vu_mode != 0);
        char what[160];
        auto expect = [&](bool ok, const char* claim) {
            std::snprintf(what, sizeof what, "EE FPU, %s, host in %s mode: %s",
                          nearest ? "to nearest" : "as the console", in_vu_mode ? "the VU" : "the default", claim);
            check(ok, what);
        };
        expect(generated::FloatAdd(kOne, kTiny) == (nearest ? 0x3F800001u : kOne), "add");
        expect(generated::FloatSub(kMinusOne, kTiny) == (nearest ? 0xBF800001u : kMinusOne), "sub");
        expect(generated::FloatMul(kOneAndHalf, kOneAndHalfUp) == (nearest ? 0x40100001u : 0x40100000u), "mul");
        expect(generated::FloatDiv(kOne, kThree) == (nearest ? 0x3EAAAAABu : 0x3EAAAAAAu), "div");
    }
}

}  // namespace

void FloatModeTests(Check check) {
    HostFloatMode(check);
    MacroMode(check);
    EeFpu(check);
}

}  // namespace ps2rt_tests
