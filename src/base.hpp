#pragma once

#include <type_traits>
#include <limits>
#include <numbers>

constexpr size_t operator""_KiB(size_t b) {
    return b * 1024llu;
}
constexpr size_t operator""_KB(size_t b) {
    return b * 1000llu;
}
constexpr size_t operator""_MiB(size_t b) {
    return b * 1024llu * 1024llu;
}
constexpr size_t operator""_MB(size_t b) {
    return b * 1000llu * 1000llu;
}
constexpr size_t operator""_GiB(size_t b) {
    return b * 1024llu * 1024llu * 1024llu;
}
constexpr size_t operator""_GB(size_t b) {
    return b * 1000llu * 1000llu * 1000llu;
}

template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

template<Numeric T>
constexpr T Abs(T value) {
    return value < T(0) ? -value : value;
}

template<Numeric T>
constexpr T Mod(T value, T step = T(1)) {
    const auto i = static_cast<long long>(value / step);
    return value - (static_cast<T>(i) * step);
}

template<Numeric T>
constexpr T Floor(T value, T step = T(1)) {
    if (step == T(0)) {
        return value;
    }

    if constexpr (std::is_integral_v<T>) {
        T q = value / step;
        T r = value % step;
        if (r != T(0) && ((r < T(0)) != (step < T(0)))) {
            q -= T(1);
        }
        return q * step;
    } else {
        T q = value / step;
        const auto i = static_cast<long long>(q);
        T t = static_cast<T>(i);
        if (t > q) {
            t -= T(1);
        }
        return t * step;
    }
}

template<Numeric T>
constexpr T Ceil(T value, T step = T(1)) {
    if (step == T(0)) {
        return value;
    }

    if constexpr (std::is_integral_v<T>) {
        T q = value / step;
        T r = value % step;
        if (r != T(0) && ((r < T(0)) == (step < T(0)))) {
            q += T(1);
        }
        return q * step;
    } else {
        T q = value / step;
        const auto i = static_cast<long long>(q);
        T t = static_cast<T>(i);
        if (t < q) {
            t += T(1);
        }
        return t * step;
    }
}

template<Numeric T>
constexpr T Round(T value, T step = T(1)) {
    if (step == T(0)) {
        return value;
    }

    if constexpr (std::is_integral_v<T>) {
        T absStep = Abs(step);
        T flo = Floor(value, step);
        T diff = value - flo;
        if (diff * T(2) >= absStep) {
            return flo + (step < T(0) ? -absStep : absStep);
        }
        return flo;
    } else {
        T q = value / step;
        T sign = q < T(0) ? T(-1) : T(1);
        T rounded = Floor(Abs(q) + T(0.5));
        return sign * rounded * step;
    }
}

template<Numeric T>
constexpr T Wrap(T value, T min, T max) {
    T range = max - min;
    if (range == T(0)) {
        return min;
    }

    if constexpr (std::is_integral_v<T>) {
        T result = (value - min) % range;
        if (result < T(0)) {
            result += Abs(range);
        }
        return result + min;
    } else {
        T step = Abs(range);
        T result = Mod(value - min, step);
        if (result < T(0)) {
            result += step;
        }
        return result + min;
    }
}

template<Numeric T>
constexpr T Ln(T x) {
    using F = std::conditional_t<std::is_floating_point_v<T>, T, double>;
    F fx = static_cast<F>(x);

    if (fx <= F(0)) {
        return static_cast<T>(-std::numeric_limits<F>::infinity());
    }

    int k = 0;
    while (fx >= F(2)) {
        fx /= F(2);
        ++k;
    }
    while (fx < F(1)) {
        fx *= F(2);
        --k;
    }

    F u = (fx - F(1)) / (fx + F(1));
    F u2 = u * u;
    F term = u;
    F sum = u;
    for (int n = 1; n < 20; ++n) {
        term *= u2;
        sum += term / F((2 * n) + 1);
    }
    return static_cast<T>((F(2) * sum) + (F(k) * std::numbers::ln2_v<long double>));
}

template<Numeric T>
constexpr T Exp(T x) {
    using F = std::conditional_t<std::is_floating_point_v<T>, T, double>;
    F fx = static_cast<F>(x);

    bool neg = fx < F(0);
    if (neg) {
        fx = -fx;
    }

    int k = 0;
    while (fx > F(0.5)) {
        fx /= F(2);
        ++k;
    }

    F term = F(1);
    F sum = F(1);
    for (int n = 1; n < 20; ++n) {
        term *= fx / F(n);
        sum += term;
    }
    for (int i = 0; i < k; ++i) {
        sum *= sum;
    }

    return static_cast<T>(neg ? F(1) / sum : sum);
}

template<Numeric T>
constexpr T Pow(T power, T exponent) {
    if constexpr (std::is_floating_point_v<T>) {
        if (exponent == static_cast<T>(static_cast<long long>(exponent))) {
            auto e = static_cast<long long>(exponent);
            bool neg = e < 0;
            if (neg) {
                e = -e;
            }
            T r(1);
            for (long long i = 0; i < e; ++i) {
                r *= power;
            }
            return neg ? T(1) / r : r;
        }
        return Exp(exponent * Ln(power));
    } else {
        T r(1);
        T e = Abs(exponent);
        for (T i = 0; i < e; ++i) {
            r *= power;
        }
        return exponent < T(0) ? T(1) / r : r;
    }
}
