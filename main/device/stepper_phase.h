#pragma once
#include <cstdint>
#include <cstdlib>

// 74HC595 U2 net names from schematic:
//   Q0=M2_L4  Q1=M1_L1  Q2=M1_L2  Q3=M1_L3
//   Q4=M1_L4  Q5=M2_L1  Q6=M2_L2  Q7=M2_L3
// Phase alias: L1=A, L2=B, L3=C, L4=D
// Axis: M2 = left/right (horizontal), M1 = up/down (vertical)
namespace stepper_phase {

inline uint8_t CoilMaskH(bool a, bool b, bool c, bool d) {
    // M2: A=Q5, B=Q6, C=Q7, D=Q0
    return (d ? 0x01u : 0u) | (a ? 0x20u : 0u) | (b ? 0x40u : 0u) | (c ? 0x80u : 0u);
}
inline uint8_t CoilMaskV(bool a, bool b, bool c, bool d) {
    // M1: A=Q1, B=Q2, C=Q3, D=Q4
    return (a ? 0x02u : 0u) | (b ? 0x04u : 0u) | (c ? 0x08u : 0u) | (d ? 0x10u : 0u);
}

// half-step 8 entries: A, AB, B, BC, C, CD, D, DA
inline uint8_t HalfStepH(int idx) {
    static const uint8_t t[8] = {
        CoilMaskH(1, 0, 0, 0), CoilMaskH(1, 1, 0, 0), CoilMaskH(0, 1, 0, 0), CoilMaskH(0, 1, 1, 0),
        CoilMaskH(0, 0, 1, 0), CoilMaskH(0, 0, 1, 1), CoilMaskH(0, 0, 0, 1), CoilMaskH(1, 0, 0, 1),
    };
    return t[((idx % 8) + 8) % 8];
}
inline uint8_t HalfStepV(int idx) {
    static const uint8_t t[8] = {
        CoilMaskV(1, 0, 0, 0), CoilMaskV(1, 1, 0, 0), CoilMaskV(0, 1, 0, 0), CoilMaskV(0, 1, 1, 0),
        CoilMaskV(0, 0, 1, 0), CoilMaskV(0, 0, 1, 1), CoilMaskV(0, 0, 0, 1), CoilMaskV(1, 0, 0, 1),
    };
    return t[((idx % 8) + 8) % 8];
}
inline uint8_t FullStepH(int idx) {
    static const uint8_t t[4] = {
        CoilMaskH(1, 0, 0, 0),
        CoilMaskH(0, 1, 0, 0),
        CoilMaskH(0, 0, 1, 0),
        CoilMaskH(0, 0, 0, 1),
    };
    return t[((idx % 4) + 4) % 4];
}
inline uint8_t FullStepV(int idx) {
    static const uint8_t t[4] = {
        CoilMaskV(1, 0, 0, 0),
        CoilMaskV(0, 1, 0, 0),
        CoilMaskV(0, 0, 1, 0),
        CoilMaskV(0, 0, 0, 1),
    };
    return t[((idx % 4) + 4) % 4];
}

inline void SelfTest() {
    if (CoilMaskH(1, 0, 0, 0) != 0x20)
        abort();  // A → Q5
    if (CoilMaskH(0, 0, 0, 1) != 0x01)
        abort();  // D → Q0
    if (CoilMaskV(1, 0, 0, 0) != 0x02)
        abort();  // A → Q1
    if (CoilMaskV(0, 0, 0, 1) != 0x10)
        abort();  // D → Q4
}

}  // namespace stepper_phase
