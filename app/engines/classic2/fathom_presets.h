// SPDX-License-Identifier: GPL-3.0-only
// FATHOM presets (included by fathom.c). Values not listed keep the parameter defaults (see the PS table).
// LV_LEVEL is calibrated by `./test_classic2 --calib` so one note at velocity 100 peaks near -6.5 dBFS.
// F_MODE: 0 = LP, 64 = NOTCH, 127 = HP. F_ENV is bipolar (-64..63).
#define FE(a, d, s, r) FE_A, a, FE_A + 1, d, FE_A + 2, s, FE_A + 3, r
#define AE(a, d, s, r) AE_A, a, AE_A + 1, d, AE_A + 2, s, AE_A + 3, r
#define ARP16 "{\"mode\":0,\"rate\":4,\"oct\":1,\"gate\":40}"
#define ARP16UD "{\"mode\":2,\"rate\":4,\"oct\":2,\"gate\":35}"
static const va_preset PRESETS[] = {
    PR("INIT", "Init", 0, NULL, LV_LEVEL, 100),
    /* ---- Bass */
    PR("Punch Bass", "Bass", 0, NULL, O1_PULSE, 1, O1_PW, 50, O2_FREQ, -12, O2_DET, 3, MX_1, 110, MX_2, 100,
       F_CUT, 40, F_RES, 36, F_ENV, 40, FE(9, 35, 10, 26), FE_A + 4, 50, AE(9, 44, 90, 20), V_DETUNE, 10,
       C_WARM, 70, C_SLOP, 14, LV_SPREAD, 10, LV_LEVEL, 105),
    PR("Fat Roller", "Bass", 0, NULL, O2_FREQ, 0, O2_DET, 6, MX_1, 127, MX_2, 127, F_CUT, 36, F_RES, 44, F_ENV, 46,
       FE(9, 28, 0, 21), FE_A + 4, 50, AE(9, 34, 20, 19), AE_A + 4, 40, V_DETUNE, 8, C_WARM, 80, C_SLOP, 10,
       LV_SPREAD, 12, LV_LEVEL, 121),
    PR("Round Sub", "Bass", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O1_PW, 64, O2_FREQ, -12, O2_SAW, 0, O2_PULSE, 1, MX_1, 90,
       MX_2, 110, F_CUT, 46, F_RES, 16, F_ENV, 20, FE(9, 41, 30, 26), AE(9, 41, 127, 21), V_DETUNE, 0,
       C_WARM, 64, C_SLOP, 12, LV_SPREAD, 0, LV_LEVEL, 103),
    PR("Notch Acid", "Bass", 0, NULL, MX_2, 0, F_CUT, 40, F_RES, 110, F_MODE, 30, F_ENV, 52, FE(9, 32, 0, 21),
       FE_A + 4, 70, AE(9, 41, 110, 20), V_MODE, 1, V_GLIDE, 44, V_LEGATO, 1, C_WARM, 100, C_SLOP, 6,
       MOD(1, MS_AT, D_MODE, 30), LV_SPREAD, 0, LV_LEVEL, 94),
    PR("Sync Bass", "Bass", 0, NULL, O1_FREQ, -12, O2_FREQ, 0, O2_SYNC, 1, MX_1, 70, MX_2, 120, F_CUT, 52, F_RES, 30,
       F_ENV, 36, FE(9, 34, 16, 21), AE(9, 41, 100, 19), V_MODE, 1, V_GLIDE, 24, C_WARM, 80,
       MOD(1, MS_FENV, D_P2, 50), MOD(2, MS_WHEEL, D_P2, 70), LV_SPREAD, 0, LV_LEVEL, 112),
    /* ---- Lead */
    PR("Bright Stack", "Lead", 0, NULL, O2_DET, 10, F_CUT, 92, F_RES, 20, F_ENV, 20, FE(9, 45, 60, 31),
       AE(10, 41, 120, 31), V_MODE, 2, V_STACK, 8, V_DETUNE, 46, V_GLIDE, 24, V_LEGATO, 1, C_WARM, 60,
       LV_SPREAD, 84, LV_LEVEL, 104),
    PR("Sync Scream", "Lead", 0, NULL, O2_FREQ, 12, O2_SYNC, 1, MX_1, 50, MX_2, 120, F_CUT, 86, F_RES, 40, F_ENV, 24,
       FE(9, 47, 40, 31), AE(10, 41, 120, 31), V_MODE, 1, V_GLIDE, 30, V_LEGATO, 1, C_WARM, 90, L1_W + 3, 80,
       MOD(1, MS_FENV, D_P2, 40), MOD(2, MS_AT, D_P2, 30), LV_SPREAD, 0, LV_LEVEL, 103),
    PR("Notch Lead", "Lead", 0, NULL, O1_PULSE, 1, O1_SAW, 0, O1_PW, 40, O2_DET, 12, F_CUT, 74, F_RES, 70,
       F_MODE, 64, F_ENV, 30, FE(9, 49, 40, 31), AE(11, 41, 120, 31), V_MODE, 1, V_GLIDE, 34, V_LEGATO, 1,
       L1_PWM, 40, L1_W + 1, 70, MOD(1, MS_WHEEL, D_MODE, 40), LV_SPREAD, 30, LV_LEVEL, 100),
    PR("Soft Saw Lead", "Lead", 0, NULL, O2_DET, 5, F_CUT, 70, F_RES, 16, F_ENV, 26, FE(16, 49, 50, 31),
       AE(15, 45, 120, 37), V_MODE, 1, V_GLIDE, 40, V_LEGATO, 1, L1_PITCH, 20, L1_W + 3, 90, C_WARM, 50,
       LV_SPREAD, 20, LV_LEVEL, 115),
    /* ---- Pad */
    PR("Warm Cloud", "Pad", 0, NULL, O2_DET, 14, F_CUT, 62, F_RES, 18, F_ENV, 18, FE(49, 65, 70, 58),
       AE(49, 58, 120, 58), V_DETUNE, 50, C_WARM, 56, C_SLOP, 50, LV_SPREAD, 84, L1_PWM, 0,
       MOD(1, MS_LFO2, D_CUT, 8), LV_LEVEL, 107),
    PR("Wide Silk", "Pad", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O2_SAW, 0, O2_PULSE, 1, O2_DET, 9, F_CUT, 70, F_RES, 26,
       F_ENV, 12, FE(58, 65, 80, 58), AE(54, 60, 124, 63), L1_W + 1, 50, L1_PWM, 70, V_DETUNE, 60, C_SLOP, 50,
       LV_SPREAD, 84, LV_LEVEL, 97),
    PR("HP Air Pad", "Pad", 0, NULL, O2_DET, 11, MX_N, 20, F_CUT, 60, F_RES, 40, F_MODE, 120, F_ENV, 10,
       FE(65, 69, 80, 63), AE(65, 65, 124, 67), L2_W + 1, 34, MOD(1, MS_LFO2, D_CUT, 14), V_DETUNE, 50,
       C_SLOP, 60, LV_SPREAD, 84, LV_LEVEL, 89),
    PR("Morph Pad", "Pad", 0, NULL, O2_DET, 13, F_CUT, 64, F_RES, 56, F_MODE, 20, F_ENV, 14, FE(58, 69, 70, 63),
       AE(58, 65, 124, 67), L2_W + 1, 30, L2_W, 6, MOD(1, MS_LFO2, D_MODE, 40), MOD(2, MS_LFO2, D_CUT, 10),
       V_DETUNE, 50, C_SLOP, 54, LV_SPREAD, 84, LV_LEVEL, 90),
    /* ---- Pluck */
    PR("Marimba Pluck", "Pluck", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O1_PW, 64, O2_FREQ, 12, O2_SAW, 0, O2_PULSE, 1,
       MX_2, 60, F_CUT, 44, F_RES, 30, F_ENV, 50, FE(9, 31, 0, 26), FE_A + 4, 60, AE(9, 42, 0, 33), AE_A + 4, 60,
       C_SLOP, 20, LV_SPREAD, 80, LV_LEVEL, 117),
    PR("BP Pluck", "Pluck", 0, NULL, O2_DET, 7, MX_1, 120, MX_2, 120, F_CUT, 56, F_RES, 50, F_BP, 1, F_ENV, 44, FE(9, 32, 0, 29),
       AE(9, 44, 0, 37), C_WARM, 70, C_SLOP, 24, LV_SPREAD, 84, LV_LEVEL, 110),
    /* ---- Keys */
    PR("Poly Keys", "Keys", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O1_PW, 30, O2_FREQ, 12, O2_DET, 4, MX_2, 50, F_CUT, 58,
       F_RES, 14, F_ENV, 30, FE(9, 47, 30, 34), FE_A + 4, 70, AE(9, 60, 64, 37), AE_A + 4, 70, C_SLOP, 24,
       LV_SPREAD, 80, LV_LEVEL, 110),
    PR("Notch Organ", "Keys", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O1_PW, 64, O2_FREQ, 12, O2_SAW, 0, O2_PULSE, 1,
       O2_PW, 64, MX_2, 80, F_CUT, 70, F_RES, 30, F_MODE, 64, F_ENV, 0, AE(9, 41, 127, 17), L1_W + 1, 92,
       L1_PITCH, 6, C_SLOP, 20, LV_SPREAD, 70, LV_LEVEL, 108),
    /* ---- Arp */
    PR("Wide Psy Arp", "Arp", JF_ARP, ARP16, O2_DET, 9, F_CUT, 50, F_RES, 44, F_ENV, 40, FE(9, 26, 0, 21),
       AE(9, 31, 0, 21), C_WARM, 60, C_SLOP, 10, LV_SPREAD, 84, LV_LEVEL, 117),
    PR("Notch Arp", "Arp", JF_ARP, ARP16UD, O1_SAW, 0, O1_PULSE, 1, O1_PW, 30, O2_FREQ, 12, MX_2, 60, F_CUT, 60,
       F_RES, 70, F_MODE, 50, F_ENV, 36, FE(9, 26, 0, 20), AE(9, 28, 0, 20), C_SLOP, 8, LV_SPREAD, 84,
       LV_LEVEL, 100),
    /* ---- Bell */
    PR("XMod Bell", "Bell", 0, NULL, O2_FREQ, 19, O2_DET, 0, O2_XMOD, 70, MX_1, 0, MX_2, 110, O2_SAW, 0, O2_PULSE, 1,
       F_CUT, 96, F_RES, 10, F_ENV, 10, FE(9, 56, 0, 51), AE(9, 67, 0, 58), C_WARM, 30, C_SLOP, 10, LV_SPREAD, 84,
       MOD(1, MS_FENV, D_XMOD, 30), LV_LEVEL, 126),
    PR("Sync Bells", "Bell", 0, NULL, O2_FREQ, 24, O2_SYNC, 1, O2_SAW, 0, O2_PULSE, 1, O2_PW, 30, MX_1, 50, MX_2, 127,
       F_CUT, 80, F_RES, 40, F_MODE, 90, F_ENV, 20, FE(9, 52, 0, 47), AE(9, 62, 0, 56), C_SLOP, 10, LV_SPREAD, 84,
       LV_LEVEL, 116),
    /* ---- Strings */
    PR("String Machine", "Strings", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O1_PW, 40, O2_DET, 14, F_CUT, 74, F_RES, 8,
       F_ENV, 0, AE(41, 58, 124, 51), L1_W + 1, 50, L1_PWM, 60, L1_PITCH, 4, V_DETUNE, 60, C_SLOP, 60,
       LV_SPREAD, 84, LV_LEVEL, 100),
    PR("Velvet Strings", "Strings", 0, NULL, O2_DET, 12, F_CUT, 66, F_RES, 14, F_ENV, 16, FE(49, 65, 80, 52),
       AE(49, 62, 124, 56), L1_W + 3, 96, L1_PITCH, 8, L1_W + 1, 84, V_DETUNE, 56, C_WARM, 50, C_SLOP, 56,
       LV_SPREAD, 84, MOD(1, MS_AT, D_CUT, 18), LV_LEVEL, 110),
    /* ---- Brass */
    PR("Big Brass", "Brass", 0, NULL, O2_DET, 9, F_CUT, 54, F_RES, 18, F_ENV, 40, FE(28, 44, 76, 31), FE_A + 4, 50,
       AE(19, 41, 120, 31), V_DETUNE, 44, C_WARM, 70, C_SLOP, 40, LV_SPREAD, 84, LV_LEVEL, 106),
    PR("Stab Brass", "Brass", 0, NULL, O2_DET, 11, F_CUT, 50, F_RES, 24, F_ENV, 46, FE(20, 38, 40, 26), FE_A + 4, 60,
       AE(14, 41, 100, 23), V_DETUNE, 40, C_WARM, 80, C_SLOP, 30, LV_SPREAD, 84, LV_LEVEL, 117),
    PR("Synthwave Horn", "Brass", 0, NULL, O1_PULSE, 1, O1_PW, 46, O2_DET, 12, F_CUT, 56, F_RES, 30, F_ENV, 42,
       FE(32, 45, 70, 33), AE(24, 44, 122, 33), L1_W + 3, 90, L1_PITCH, 10, V_DETUNE, 54, C_WARM, 64, C_SLOP, 44,
       LV_SPREAD, 84, LV_LEVEL, 94),
    /* ---- Drone */
    PR("Dark Ocean", "Drone", JF_HOLD, NULL, O2_FREQ, -12, O2_DET, 6, MX_N, 16, F_CUT, 48, F_RES, 60, F_MODE, 16,
       F_ENV, 0, AE(74, 58, 127, 75), L2_W + 1, 24, MOD(1, MS_LFO2, D_CUT, 20), MOD(2, MS_LFO1, D_MODE, 14),
       L1_W + 1, 36, V_DETUNE, 60, C_WARM, 70, C_SLOP, 80, LV_SPREAD, 84, LV_LEVEL, 119),
    PR("Breath Drone", "Drone", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O1_PW, 70, O2_FREQ, 12, O2_DET, 4, MX_2, 70,
       F_CUT, 56, F_RES, 50, F_BP, 1, F_ENV, 0, AE(65, 58, 127, 58), L2_W + 1, 44, MOD(1, MS_LFO2, D_CUT, 14),
       L1_PWM, 50, L1_W + 1, 40, C_SLOP, 60, LV_SPREAD, 84, LV_LEVEL, 108),
    /* ---- Texture */
    PR("Wind Band", "Texture", 0, NULL, MX_1, 0, MX_2, 0, MX_N, 127, F_CUT, 64, F_RES, 56, F_BP, 1, F_ENV, 0, F_KEY, 0,
       AE(49, 58, 127, 67), C_WARM, 110, L2_W + 1, 36, L2_W + 4, 1, MOD(1, MS_LFO2, D_CUT, 24), MOD(2, MS_RAND, D_CUT, 16),
       C_SLOP, 90, LV_SPREAD, 84, LV_LEVEL, 127),
    PR("Morph Mist", "Texture", 0, NULL, O1_SAW, 0, O1_PULSE, 1, O2_FREQ, 7, O2_XMOD, 30, MX_N, 20, F_CUT, 60,
       F_RES, 60, F_MODE, 40, F_ENV, 10, AE(69, 65, 120, 70), L2_W, 6, L2_W + 1, 40, L2_W + 4, 1,
       MOD(1, MS_LFO2, D_MODE, 50), MOD(2, MS_LFO1, D_XMOD, 20), L1_W + 1, 30, C_SLOP, 70, LV_SPREAD, 84,
       LV_LEVEL, 99),
    /* ---- FX */
    PR("Sweep Zap", "FX", 0, NULL, O2_FREQ, 12, O2_SYNC, 1, F_CUT, 90, F_RES, 50, F_ENV, 20, FE(9, 37, 0, 31),
       AE(9, 45, 0, 33), MOD(1, MS_FENV, D_P2, 100), MOD(2, MS_FENV, D_PITCH, 60), C_SLOP, 0, LV_SPREAD, 40,
       LV_LEVEL, 107),
    PR("Siren", "FX", 0, NULL, O1_PULSE, 1, O1_SAW, 1, MX_1, 120, MX_2, 0, F_CUT, 86, F_RES, 40, F_ENV, 0, AE(13, 41, 127, 37),
       L1_W, 1, L1_W + 1, 60, L1_PITCH, 70, MOD(1, MS_WHEEL, D_L1RATE, 40), C_SLOP, 0, LV_SPREAD, 0, LV_LEVEL, 112),
    /* ---- Sequence */
    PR("Techno Seq", "Sequence", 0, NULL, O1_PULSE, 1, O1_SAW, 0, O1_PW, 36, O2_FREQ, 12, O2_DET, 5, MX_2, 60,
       F_CUT, 46, F_RES, 56, F_ENV, 44, FE(9, 28, 0, 21), FE_A + 4, 60, AE(9, 33, 0, 21), C_WARM, 70, C_SLOP, 8,
       LV_SPREAD, 60, LV_LEVEL, 100),
    PR("BP Seq", "Sequence", 0, NULL, O2_DET, 9, MX_1, 127, MX_2, 127, F_CUT, 60, F_RES, 64, F_BP, 1, F_ENV, 34, FE(9, 27, 0, 21),
       AE(9, 32, 0, 21), MOD(1, MS_RAND, D_CUT, 12), C_WARM, 60, C_SLOP, 8, LV_SPREAD, 80, LV_LEVEL, 115),
};
#undef FE
#undef AE
