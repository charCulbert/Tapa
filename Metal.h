#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace bd
{
class Metal
{
public:
    void trigger(double rate, double baseHz, double ratio, double amount,
                 double decayMs, double feedback, double strike) noexcept
    {
        for (size_t i = 0; i < modes.size(); ++i)
        {
            auto& mode = modes[i];
            const auto spacing = std::pow(partials[i], .8 + .4 * amount);
            const auto frequency = baseHz * std::pow(ratio, .25) * spacing;
            const auto duration = decayMs * (.35 + .65 * feedback)
                                / std::pow(partials[i], .12 + .25 * (1.0 - strike));
            const auto radius = std::exp(-6.90775527898 / (duration * .001 * rate));
            const auto angle = 6.283185307179586 * frequency / rate;
            mode = {radius * std::cos(angle), radius * std::sin(angle),
                    std::sqrt(48000.0 / rate) * .035
                        * std::clamp((rate * .105 - frequency) / (rate * .025), 0.0, 1.0), 0, 0};
        }
    }

    void reset() noexcept { for (auto& mode : modes) mode.real = mode.imaginary = 0; }

    double next(double excitation) noexcept
    {
        double sum = 0;
        for (auto& mode : modes)
        {
            const auto real = mode.cosine * mode.real - mode.sine * mode.imaginary
                            + mode.gain * excitation;
            mode.imaginary = mode.sine * mode.real + mode.cosine * mode.imaginary;
            mode.real = real;
            sum += real;
        }
        return sum;
    }

private:
    struct Mode { double cosine = 0, sine = 0, gain = 0, real = 0, imaginary = 0; };
    // Inharmonic modes; upper modes fade below the output Nyquist frequency.
    static constexpr std::array partials {1.0, 1.37, 1.83, 2.31, 2.93, 3.71,
        4.19, 5.13, 5.89, 6.73, 7.91, 9.17, 10.43, 11.89, 13.37, 15.17, 17.29, 19.73};
    std::array<Mode, partials.size()> modes {};
};
}
