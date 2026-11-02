#pragma once

// Small filters shared by the waveguide voices (flute, strings, horn).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>

namespace pianocraft::filters {

constexpr double pi = 3.141592653589793238462643383279502884;

// Phase delay in samples of (b0 + b1 z^-1) / (1 - a z^-1) at omega.
inline double phaseDelay(double b0, double b1, double a, double omega) {
    const double numerator = std::atan2(-b1 * std::sin(omega), b0 + b1 * std::cos(omega));
    const double denominator = std::atan2(a * std::sin(omega), 1.0 - a * std::cos(omega));
    return -(numerator - denominator) / omega;
}

// RBJ biquads {b0, b1, b2, a1, a2}: Butterworth high- or low-pass at corner (Hz) (another Q makes
// the second-order section of a higher-order Butterworth), and a band-pass with unit peak gain.
inline std::array<double, 5> butterworth(bool highpass, double corner, double fs, double q = 0.7071067811865476) {
    const double w = 2.0 * pi * std::min(corner, 0.45 * fs) / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    const double b0 = (highpass ? 1.0 + c : 1.0 - c) / 2.0;
    return {b0 / a0, (highpass ? -2.0 : 2.0) * b0 / a0, b0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

// RBJ resonant low-pass: unit gain at DC, a peak of about Q at the corner.
inline std::array<double, 5> resonance(double corner, double q, double fs) {
    const double w = 2.0 * pi * std::min(corner, 0.45 * fs) / fs, c = std::cos(w), alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    const double b0 = (1.0 - c) / 2.0;
    return {b0 / a0, 2.0 * b0 / a0, b0 / a0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

inline std::array<double, 5> bandpass(double centre, double q, double fs) {
    const double w = 2.0 * pi * std::min(centre, 0.45 * fs) / fs, alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    return {alpha / a0, 0.0, -alpha / a0, -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
}

inline double magnitude(const std::array<double, 5>& q, double omega) {
    const double c1 = std::cos(omega), s1 = std::sin(omega), c2 = std::cos(2.0 * omega), s2 = std::sin(2.0 * omega);
    return std::hypot(q[0] + q[1] * c1 + q[2] * c2, q[1] * s1 + q[2] * s2) / std::hypot(1.0 + q[3] * c1 + q[4] * c2, q[3] * s1 + q[4] * s2);
}

// Transposed direct form II; z1 and z2 are the state.
inline double biquad(const std::array<double, 5>& q, double x, double& z1, double& z2) {
    const double y = q[0] * x + z1;
    z1 = q[1] * x - q[3] * y + z2;
    z2 = q[2] * x - q[4] * y;
    return y;
}

// Cubic Lagrange read, delay samples behind write, of a ring buffer of mask + 1 (a power of two).
template <class T>
double lagrange(const T* line, std::size_t mask, std::size_t write, double delay) {
    const double position = static_cast<double>(write + mask + 1) - delay;
    const auto base = static_cast<std::size_t>(position);
    const double d = position - static_cast<double>(base);
    const double ym1 = line[(base - 1) & mask], y0 = line[base & mask], y1 = line[(base + 1) & mask], y2 = line[(base + 2) & mask];
    return y0 + d * (0.5 * (y1 - ym1) + d * (ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2 + d * (1.5 * (y0 - y1) + 0.5 * (y2 - ym1))));
}

// A fitted output stage (tools/iowa_calibration.py): a Butterworth high-pass of order 1 to 3 (0: none)
// at `highpass` Hz, three one-pole low-passes at tone x (1 + bloom breath^3) x (f0 / C3)^track Hz
// (tone 0: none), and a gain.
struct Voicing {
    int order;
    double highpass, tone, bloom, track, gain;
};

class VoicingStage {
public:
    void block(const Voicing& v, double breath, double f0, double fs) {
        rate_ = 1.0 - std::exp(-2.0 * pi * v.highpass / fs);
        section_ = butterworth(true, v.highpass, fs, v.order == 3 ? 1.0 : 0.7071067811865476);
        const double corner = v.tone * (1.0 + v.bloom * breath * breath * breath) * std::pow(f0 / 130.81, v.track);
        lp_ = v.tone > 0.0 ? 1.0 - std::exp(-2.0 * pi * std::min(corner, 0.4 * fs) / fs) : 1.0;
        order_ = v.order;
        gain_ = v.gain;
    }
    double process(double x) {
        if (order_ == 1 || order_ == 3) {
            s_[0] += rate_ * (x - s_[0]);
            x -= s_[0];
        }
        if (order_ >= 2) {
            x = biquad(section_, x, s_[1], s_[2]);
        }
        for (std::size_t i = 3; i < 6; ++i) {
            x = s_[i] += lp_ * (x - s_[i]);
        }
        return gain_ * x;
    }
    void reset() { s_ = {}; }

private:
    std::array<double, 5> section_{};
    std::array<double, 6> s_{};
    double rate_ = 0.0, lp_ = 1.0, gain_ = 1.0;
    int order_ = 0;
};

} // namespace pianocraft::filters
