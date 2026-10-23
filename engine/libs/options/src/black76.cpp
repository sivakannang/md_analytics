#include <algorithm>
#include <cmath>

#include <mda/options/black76.hpp>

namespace mda::options {
namespace {

constexpr double kInvSqrt2Pi = 0.398942280401432677940;

double norm_cdf(double x) {
    return 0.5 * std::erfc(-x / std::sqrt(2.0));
}

double norm_pdf(double x) {
    return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

struct D {
    double d1;
    double d2;
};

D d_terms(double forward, double strike, double years, double sigma) {
    const double sd = sigma * std::sqrt(years);
    const double d1 = (std::log(forward / strike) + 0.5 * sd * sd) / sd;
    return {d1, d1 - sd};
}

bool valid(double forward, double strike, double years, double sigma) {
    return forward > 0 && strike > 0 && years > 0 && sigma > 0 && std::isfinite(forward) && std::isfinite(sigma);
}

}  // namespace

double black76_price(double forward, double strike, double years, double sigma, bool call) {
    if (!valid(forward, strike, years, sigma)) {
        return call ? std::max(forward - strike, 0.0) : std::max(strike - forward, 0.0);
    }
    const auto [d1, d2] = d_terms(forward, strike, years, sigma);
    return call ? forward * norm_cdf(d1) - strike * norm_cdf(d2) : strike * norm_cdf(-d2) - forward * norm_cdf(-d1);
}

Greeks black76_greeks(double forward, double strike, double years, double sigma, bool call) {
    if (!valid(forward, strike, years, sigma)) {
        return {};
    }
    const auto [d1, d2] = d_terms(forward, strike, years, sigma);
    (void)d2;
    const double sqrt_t = std::sqrt(years);
    const double pdf = norm_pdf(d1);
    return {.delta = call ? norm_cdf(d1) : norm_cdf(d1) - 1.0,
            .gamma = pdf / (forward * sigma * sqrt_t),
            .theta = -(forward * pdf * sigma / (2.0 * sqrt_t)) / 365.0,
            .vega = forward * pdf * sqrt_t / 100.0};
}

std::optional<double> implied_vol(double price, double forward, double strike, double years, bool call) {
    if (!(price > 0) || forward <= 0 || strike <= 0 || years <= 0) {
        return std::nullopt;
    }
    const double intrinsic = call ? std::max(forward - strike, 0.0) : std::max(strike - forward, 0.0);
    const double ceiling = call ? forward : strike;
    if (price <= intrinsic + 1e-9 || price >= ceiling) {
        return std::nullopt;  // no volatility produces this price
    }
    // Newton from 20%; falls back to bisection when a step leaves (0, 5] or vega vanishes.
    double sigma = 0.2;
    for (int i = 0; i < 30; ++i) {
        const double diff = black76_price(forward, strike, years, sigma, call) - price;
        if (std::abs(diff) < 1e-6) {
            return sigma;
        }
        const double vega = black76_greeks(forward, strike, years, sigma, call).vega * 100.0;
        if (vega < 1e-10) {
            break;
        }
        const double next = sigma - diff / vega;
        if (!(next > 0 && next <= 5.0)) {
            break;
        }
        sigma = next;
    }
    double low = 1e-4;
    double high = 5.0;
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (low + high);
        const double diff = black76_price(forward, strike, years, mid, call) - price;
        if (std::abs(diff) < 1e-6) {
            return mid;
        }
        (diff > 0 ? high : low) = mid;
    }
    return 0.5 * (low + high);
}

}  // namespace mda::options
