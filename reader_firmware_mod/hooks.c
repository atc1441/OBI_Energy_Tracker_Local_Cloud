// hooks.c — hook bodies, compiled as normal freestanding C (AAPCS calling
// convention). Register-convention glue lives in entry.S, not here.
#include "vendor.h"

// int24 (SML TL=0x54) decoder — the reader_meter_v57.bin firmware is
// missing this case in its generic SML value-type switch (sub_C0A4),
// which is why power (16.7.0) shows n/a whenever the meter's value needs
// 3 bytes to encode. This restores it, compiled from C instead of the
// hand-assembled Thumb-1 trampoline used for the very first fix (proven
// byte-for-byte equivalent via IDA diff before this pipeline replaced it).
//
//   p   = pointer to the TL byte of the value field (== R5+R2 at the
//         original call site)
//   out = pointer to the 10-byte output area (== R4 at the original call
//         site): out[0..3] = value (low dword), out[4..7] = sign dword,
//         out[8]/out[9] = the two extra bytes the caller also expects
//         (unit/scaler-adjacent bytes, copied from p-0x20+0x1D / +0x1F)
//
// Returns 1 (the vendor's "decode succeeded" convention for this switch
// case) -- entry.S puts this return value straight into R0 for the caller.
//
int hook_decode_int24(u8 *p, u8 *out)
{
    u32 v = ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    i32 value = (i32)(v << 8) >> 8;      // sign-extend 24 -> 32

    i32 hi    = value >> 31;             // sign-extended hi dword

    out[0] = (u8)(value);
    out[1] = (u8)(value >> 8);
    out[2] = (u8)(value >> 16);
    out[3] = (u8)(value >> 24);
    out[4] = (u8)(hi);
    out[5] = (u8)(hi >> 8);
    out[6] = (u8)(hi >> 16);
    out[7] = (u8)(hi >> 24);

    u8 *q = p - 0x20;
    out[8] = q[0x1D];
    out[9] = q[0x1F];

#ifdef FIX_NEGATIVE_POWER
    // Mark "a 24-bit (TL=0x54) value was just decoded" so hook_power_correct2
    // (fired moments later, downstream in the same telegram) can tell WHICH
    // width the meter actually used for the value it's about to correct --
    // see the WAS_INT24_ADDR block there for why this matters.
    *((volatile u32 *)0x20001014) = 1;
#endif
    return 1;
}

#ifdef FIX_NEGATIVE_POWER
// DWSB20.2TH negative-power fix, 3rd design. Earlier versions tried to
// identify "this is OBIS 16.7.0" from *inside* hook_decode_int24 by
// inspecting bytes near the raw SML pointer (a fixed -16 offset, then a
// tighter unit/scaler-adjacent check) -- both were proven correct against
// one captured frame via a byte-for-byte Python replay, but neither fired
// reliably live, repeatedly, even after ruling out stale-OTA caching by
// bumping to a fresh never-used softver each time. Conclusion: whatever
// exact byte layout precedes the value field is not as constant as that
// one capture suggested, and guessing more offsets blind isn't converging.
//
// This version sidesteps needing that byte-level context entirely: it
// hooks sub_7434 (reader_meter_v57.bin) at 0x75EE, the `BL sub_4700` call
// that converts the raw decoded int (in R0) to the final float/int power
// value -- reached ONLY on the branch that already matched the OBIS
// string "1-0:16.7.0*255" via sub_C000 a few instructions earlier, so
// there is zero ambiguity about which field this is; no byte-scanning
// needed. Correcting the RAW value here (before sub_4700's scaler
// conversion, before it's stored to the struct) lets the corrected value
// flow through the SAME normal path every legitimate reading already
// takes -- unlike an even earlier attempt that patched sub_7434's tail
// (0x7604, AFTER the struct was already filled and about to return):
// that write never survived to the transmitted energy payload (confirmed
// with a temporary gateway-side raw-payload debug field), i.e. something
// downstream reads the struct fields via the normal flow, not a stale
// snapshot, so fixing the value while it's still in that flow (here)
// should reach it -- fixing it after the flow already committed (there)
// did not.
//
// Since only the RAW INT is available at this point (not the original
// SML bytes), the meter-side bug (top byte 0x00 instead of 0xFF, see
// README) is instead recognized via the same import/export trend check
// as the abandoned 0x7604 attempt: cross-telegram delta of the already-
// decoded import/export counters (reliable, monotonic, unaffected by
// this bug) tells us whether the current telegram should be net export
// (negative power), and the correction only applies to values small
// enough to plausibly be the corrupted case (0..65535 raw units, i.e.
// up to 655.35 W at this meter's scaler of -2).
#define OBI_NA    0x7FFFFFFF
#define NEG_FIX_RAW 65536   // raw SML units (hook_power_correct / sub_7434 path -- currently inert)
#define NEG_FIX_W   655     // Watts (hook_power_correct2 / sub_9ED8 path -- the active one). 65536 raw
                            // units * this meter's scaler (10^-2) = 655.36 W, truncated.

// SECOND bracket (2026-07-11): confirmed live that feed-in beyond roughly
// 655 W surfaces via a DIFFERENT, much larger wraparound instead of the
// NEG_FIX_W one above -- e.g. a real ~-720 W moment showed up as
// +167053..+167098 W (physically impossible for a residential meter).
// This matches the well-known 3-tier bracket pattern for this exact bug
// class (2.56 / 655.36 / 167772.16 W -- see README), i.e. 2^24 raw units
// (this meter's scaler again gives /100 = W): whatever internal width the
// meter's encoder lands on for a given magnitude, the byte that should
// carry the negative sign extension for THAT width is wrongly written as
// 0x00, and the resulting offset is 2^(that width) raw units. Values
// under ~655 W apparently widen via one extra byte (2^16 offset,
// NEG_FIX_W); confirmed live that values whose true magnitude needs the
// full 24-bit range instead land 2^24 raw units off (167772 W once
// scaled) -- reproduced via the gateway's own history log: the implied
// true values (offset by exactly -167772) matched the concurrent Wh-tick
// rate on the export counter almost exactly (e.g. -719 W implied vs.
// ~720 W average from the tick delta over that sample window).
//
// Unlike NEG_FIX_W (which DOES need the dir==2 gate -- confirmed live
// that small genuinely-positive values reach this same decoder, see
// below), this second bracket needs no such gate: no real residential
// reading ever approaches 167772 W (or even a tenth of it) under ANY
// interpretation, so a decoded value at/above IMPLAUSIBLE_W is
// unconditionally corrupted, regardless of the sticky latch's current
// state -- which also sidesteps the latch's inherent lag for this case.
#define NEG_FIX_W3     167772   // Watts. 2^24 raw units * 10^-2 scaler = 167772.16, truncated.
#define IMPLAUSIBLE_W  50000    // Watts. Comfortably above any real reading (max plausible residential
                                 // draw/feed-in is at most a few tens of kW) and comfortably below the
                                 // lowest value NEG_FIX_W3-bracket corruption can produce (~83886 W, at
                                 // the Int24 magnitude ceiling) -- see README for the derivation.

// volatile is load-bearing here: without it, a plain (u32*) let the
// compiler treat the later re-read of *PREV_MAGIC_ADDR as predictable
// from the earlier write earlier in program order and optimize/reorder
// around it -- confirmed live: the plain-pointer version never saw its
// magic word survive to the next call (always "no baseline yet"), and
// only started working once an extra, seemingly-redundant write+read was
// added right before the real check (a workaround for the same root
// cause, not a real fix). volatile makes each access a genuine memory
// operation the compiler can't reason away.
#define PREV_MAGIC_ADDR ((volatile u32 *)0x20001000)
#define PREV_IMP_ADDR   ((volatile u32 *)0x20001004)
#define PREV_EXP_ADDR   ((volatile u32 *)0x20001008)
#define PREV_MAGIC_VAL  0xDEC0DEDu

// DIAGNOSTIC CONFIRMED (2026-07-10): an unconditional sentinel here never
// showed up live -- sub_7434 (and this 0x75EE patch site) is NOT the code
// path this reader actually uses for live power reporting; the OTHER
// path (sub_77B4/sub_9ED8, see hook_power_correct2 below) is. Turned into
// a pure, scratch-free no-op (rather than removing the hook/patch site
// entirely) so it can't interact with hook_power_correct2's use of the
// same PREV_* scratch below, while still being cheap insurance in case
// some telegram variant does reach it after all.
i32 hook_power_correct(i32 raw)
{
    return raw;
}

// Same trend-correction logic, but reading/tracking the OTHER path's own
// struct (unk_20000D68: import at +0, export at +4, power at +8 -- see
// sub_77B4 in IDA). Reuses the SAME scratch addresses as
// hook_power_correct's PREV_* (0x20000D10/14/18) rather than a separate
// block at 0x20000D20: that address never held its magic word across
// calls (every single call reported "no baseline yet", confirmed live),
// so it isn't safe/free RAM after all -- something else overwrites it
// between calls. 0x20000D10 IS proven safe (worked reliably for
// hook_power_correct's own diagnostics earlier), and reusing it here is
// fine since hook_power_correct's path is confirmed dead code (its own
// sentinel never fired), so there's no real risk of the two colliding.
#define PREV2_MAGIC_ADDR PREV_MAGIC_ADDR
#define PREV2_IMP_ADDR   PREV_IMP_ADDR
#define PREV2_EXP_ADDR   PREV_EXP_ADDR

// CONFIRMED live 2026-07-10: this is the actually-reached path (an
// unconditional sentinel here showed up consistently; the sub_7434-based
// hook_power_correct's own sentinel never did) -- unk_20000D68=import,
// unk_20000D6C=export, both confirmed matching the gateway's own reported
// values exactly, and the dImp/dExp trend computation confirmed correct
// (all three sub-conditions -- baseline present, value in the corrupted
// bracket, export-dominant delta -- fired together exactly on the
// telegram where an export tick landed, verified via a flag-bits debug
// build).
//
// Upgraded from a strict "did THIS telegram's delta show export-dominant"
// check to a STICKY direction latch: the cumulative Wh counters only
// tick roughly every several seconds to minutes at typical feed-in power,
// while power itself is reported every ~6s, so requiring an exact same-
// telegram coincidence between "a counter ticked" and "power is in the
// corrupted bracket" missed the vast majority of genuinely negative
// readings (confirmed live: a real -10 W moment still showed +640 W).
// Instead, remember which counter ticked LAST (import or export) and
// keep applying the correction based on that remembered direction --
// but only for a bounded number of quiet telegrams (STICKY_MAX_AGE),
// not indefinitely: confirmed live that an unbounded latch overcorrects
// once the true value drifts back toward zero without a fresh tick to
// confirm a direction change (e.g. showing -655 W when the true reading
// had settled back near 0). This can't be fully eliminated -- a
// direction change genuinely isn't visible until the FIRST tick of the
// new direction arrives, and ticks are the only ground truth available
// here (confirmed live: ~1 minute of wrong readings during a direction
// change is the cost of relying on the Wh counters at all) -- but aging
// the latch out bounds how long a STALE direction keeps getting applied
// after the counters stop moving altogether.
#define STATE_DIR_ADDR ((volatile u32 *)0x2000100C)   // 0=unknown, 1=import last ticked, 2=export last ticked
#define STATE_AGE_ADDR ((volatile u32 *)0x20001010)   // quiet telegrams (no tick either way) since dir was last set
#define STICKY_MAX_AGE 5

// Width-aware small-magnitude rule (2026-07-11), replacing the earlier abandoned attempt: this meter's own
// encoder never sends a genuine positive reading via the 24-bit field (TL=0x54) at all, up to at least the
// full small-bracket ceiling (NEG_FIX_W, 655 W) -- positive values in that whole range go out via some
// 16-bit-family encoding (Uint16, TL=0x63) that hook_decode_int24 never sees. So if a value reaching this
// hook (a) was actually decoded via the 24-bit path (hook_decode_int24, tracked via WAS_INT24_ADDR below --
// set there, consumed here) AND (b) is still under NEG_FIX_W, the ONLY way that combination happens is the
// corrupted-negative case (top byte wrongly 0x00 instead of 0xFF) -- a genuine positive reading that small
// would never have used the 24-bit field in the first place. No dir/latch gating needed for this bracket at
// all -- unlike the tick-based fallback below, this doesn't need to wait for a Wh counter tick, which
// matters a lot at low power: a genuine -30 W export takes over 2 hours to tick even 1 Wh, so the tick-based
// approach alone could never catch it in practice. Live-verified 2026-07-11: confirmed both that small
// negative readings (e.g. -30 W, previously showing as +630 W with the tick-only approach) are now
// corrected immediately, AND that genuine positive readings across the full 0-655 W range (including the
// 330-655 W band this specific household regularly sees) are NOT misfired negative.
//
// This is deliberately NOT the same as the earlier "TRIED AND REVERTED" rule below (which checked raw
// magnitude alone, with no way to know which width had actually been used, and wrongly flipped genuine
// small positive readings that happened to reach this same post-decode hook via the 16-bit path). Gating
// on WAS_INT24_ADDR removes that ambiguity -- it only fires for values this reader ITSELF confirmed came
// through the 24-bit decoder, not merely "a small value showed up here".
#define WAS_INT24_ADDR ((volatile u32 *)0x20001014)   // set by hook_decode_int24, consumed (read+cleared) here
#define WIDTH_GATE_W    NEG_FIX_W   // this meter never sends genuine positive via 24-bit below this ceiling (confirmed live)

i32 hook_power_correct2(i32 raw)
{
    if (raw == OBI_NA) return raw;

    u32 was24 = *WAS_INT24_ADDR;
    *WAS_INT24_ADDR = 0;   // consume: only reflects a 24-bit decode since the LAST correction check

    volatile u32 *impP = (volatile u32 *)0x20000D68;
    volatile u32 *expP = (volatile u32 *)0x20000D6C;
    u32 imp = *impP;
    u32 exp = *expP;
    if (imp == OBI_NA || exp == OBI_NA) return raw;

    u32 prevMagic = *PREV2_MAGIC_ADDR;
    u32 prevImp = *PREV2_IMP_ADDR;
    u32 prevExp = *PREV2_EXP_ADDR;
    u32 dir = *STATE_DIR_ADDR;
    u32 age = *STATE_AGE_ADDR;

    if (prevMagic == PREV_MAGIC_VAL) {
        if (exp != prevExp)      { dir = 2; age = 0; }   // export ticked -> latch export, reset age
        else if (imp != prevImp) { dir = 1; age = 0; }   // import ticked (and export didn't) -> latch import
        else if (age < STICKY_MAX_AGE) age++;            // neither ticked: age the latch
        if (age >= STICKY_MAX_AGE) dir = 0;              // aged out -> back to "unknown", no correction
    }

    *PREV2_IMP_ADDR = imp;
    *PREV2_EXP_ADDR = exp;
    *PREV2_MAGIC_ADDR = PREV_MAGIC_VAL;
    *STATE_DIR_ADDR = dir;
    *STATE_AGE_ADDR = age;

    // NEG_FIX_RAW (65536) is the offset in RAW SML units (pre-scaling),
    // correct for hook_power_correct's path (sub_7434, where the value at
    // this point genuinely is the pre-scaled raw int24). sub_9ED8 (this
    // path) has no separate scaler-conversion step visible in IDA -- it
    // returns the ALREADY-SCALED final Watts value directly -- so using
    // 65536 here subtracted 65536 *Watts*, not 655.36 W, which is exactly
    // why a real -30 W moment came out as -65000 W instead (confirmed
    // live 2026-07-10). The correct offset in Watts is 65536 * 10^-2 (this
    // meter's scaler) = 655.36, i.e. NEG_FIX_W below -- a completely
    // different unit from NEG_FIX_RAW, not a smaller version of it.
    //
    // TRIED AND REVERTED (2026-07-10): an unconditional "magnitude alone
    // proves it's negative" rule for small values, on the theory that a
    // genuinely small positive reading would go out via the shorter
    // Int16 encoding and never reach this decoder at all. Confirmed live
    // this does NOT hold for sub_9ED8's path specifically: watched a
    // clearly import-dominant stretch (import ticking, export static)
    // where hook_power_correct2 still received small values (7-27), and
    // the unconditional rule flipped those genuinely-positive readings
    // negative too. So sub_9ED8 apparently receives small POSITIVE
    // values through this same path as well -- magnitude alone can't
    // distinguish them here, unlike what holds one level down in the
    // raw SML bytes. Back to trend-only, which is anchored to the
    // (reliable, if laggy) counter ticks instead.
    //
    // NEW (2026-07-11): the width-gated rule above (was24 && raw <
    // WIDTH_GATE_W) doesn't have that ambiguity -- it's checked FIRST,
    // ahead of the tick-based trend logic, since it doesn't need to wait
    // for a Wh counter tick at all (removing the "~1 minute of wrong
    // readings during a direction change" lag for this specific,
    // provably-corrupted case). The tick-based dir==2 check remains as a
    // fallback for whatever this width check doesn't catch.
    if (was24 && raw >= 0 && raw < WIDTH_GATE_W)
        raw -= NEG_FIX_W;
    else if (raw >= IMPLAUSIBLE_W)
        raw -= NEG_FIX_W3;
    else if (dir == 2 && raw >= 0 && raw < NEG_FIX_W)
        raw -= NEG_FIX_W;
    return raw;
}
#endif
