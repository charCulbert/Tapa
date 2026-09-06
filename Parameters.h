#pragma once
#include <array>
namespace tapa
{
struct ParameterInfo { unsigned id; const char* name; double minimum, maximum, defaultValue; const char* unit; };
inline constexpr std::array parameters {
    ParameterInfo { 1, "Decay", 20.0, 4000.0, 280.0, "ms" },
    ParameterInfo { 2, "FM Amount", 0.0, 100.0, 50.0, "%" },
    ParameterInfo { 3, "Transient", 0.0, 100.0, 30.0, "%" },
    ParameterInfo { 4, "Saturation", 0.0, 12.0, 2.0, "dB" },
    ParameterInfo { 5, "Feedback", 0.0, 100.0, 0.0, "%" },
    ParameterInfo { 6, "FM Ratio", 0.25, 16.0, 2.6025, "x" },
    ParameterInfo { 7, "Linked Decay", 0.0, 1.0, 0.0, "" },
    ParameterInfo { 8, "Noise", 0.0, 100.0, 0.0, "%" }
};
}
