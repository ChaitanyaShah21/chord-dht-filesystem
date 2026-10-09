#include "metrics.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

Histogram::Histogram(std::vector<double> bounds) : bounds_(std::move(bounds)) {
    // make_unique<T[]>(n) value-initialises every element: each atomic starts at 0.
    counts_ = std::make_unique<std::atomic<std::uint64_t>[]>(bounds_.size() + 1);
}

void Histogram::observe(double x) {
    // A duration or a hop count is never negative, and NaN means a bug upstream;
    // either is counted as 0 rather than allowed to wrap the unsigned sum below.
    // `!(x >= 0)` is true for NaN as well, because every comparison with NaN is false.
    if (!(x >= 0)) x = 0;

    // The first bound at or above x. Linear: at most 16 bounds, and the early ones
    // are where most observations land.
    std::size_t i = 0;
    while (i < bounds_.size() && x > bounds_[i]) ++i;
    counts_[i].fetch_add(1, std::memory_order_relaxed);

    // Capped so that x * 1e6 always fits in 64 bits (1e12 s is 31,000 years).
    const double capped = x < 1e12 ? x : 1e12;
    sum_millionths_.fetch_add(static_cast<std::uint64_t>(std::llround(capped * 1e6)), std::memory_order_relaxed);
}

Histogram::Reading Histogram::read() const {
    Reading r;
    r.counts.resize(bounds_.size() + 1);
    for (std::size_t i = 0; i <= bounds_.size(); ++i) r.counts[i] = counts_[i].load(std::memory_order_relaxed);
    r.sum = static_cast<double>(sum_millionths_.load(std::memory_order_relaxed)) / 1e6;
    return r;
}

const std::vector<double> &latency_buckets() {
    // A function-local static is built once, on first use, and C++11 guarantees
    // that is thread-safe even if two threads call this at the same moment.
    static const std::vector<double> b{0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05,
                                       0.1,    0.2,   0.5,   1,     2,    5,    10};
    return b;
}

const std::vector<double> &hop_buckets() {
    static const std::vector<double> b{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 32, 64};
    return b;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

namespace {

// Escape a label value: backslash, double quote and newline, as the format says.
std::string escape_label(const std::string &v) {
    std::string out;
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// Escape HELP text: backslash and newline only (quotes are allowed there).
std::string escape_help(const std::string &v) {
    std::string out;
    for (char c : v) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// name{labels} or name{labels,extra} -- the braces only when there is something in them.
std::string series_name(const std::string &name, const std::string &label_text, const std::string &extra = "") {
    std::string inside = label_text;
    if (!extra.empty()) inside += (inside.empty() ? "" : ",") + extra;
    return inside.empty() ? name : name + "{" + inside + "}";
}

}  // namespace

std::string labels(std::initializer_list<std::pair<std::string, std::string>> pairs) {
    std::string out;
    for (const auto &p : pairs) out += (out.empty() ? "" : ",") + p.first + "=\"" + escape_label(p.second) + "\"";
    return out;
}

bool valid_metric_name(const std::string &name) {
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !(digit && i > 0)) return false;
    }
    return true;
}

std::string format_value(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    char buf[64];
    // Integers up to 2^53 are exact in a double: print them without a fraction,
    // so a counter reads 1234, not 1234.000000 or 1.234e+03.
    if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.15g", v);
    return buf;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

void Registry::add(Series s) {
    if (!valid_metric_name(s.name)) throw std::invalid_argument("invalid metric name: " + s.name);
    for (const Series &other : series_) {
        if (other.name != s.name) continue;
        if (other.type != s.type) throw std::invalid_argument("metric " + s.name + " registered as two types");
        if (other.label_text == s.label_text) throw std::invalid_argument("metric " + s.name + " registered twice");
    }
    series_.push_back(std::move(s));
}

Counter &Registry::counter(const std::string &name, const std::string &help, const std::string &label_text) {
    counters_.push_back(std::make_unique<Counter>());
    Series s{name, help, label_text, Type::Counter};
    s.counter = counters_.back().get();
    add(std::move(s));
    return *counters_.back();
}

Histogram &Registry::histogram(const std::string &name, const std::string &help, const std::vector<double> &bounds,
                               const std::string &label_text) {
    if (bounds.empty()) throw std::invalid_argument("histogram " + name + " has no buckets");
    for (std::size_t i = 1; i < bounds.size(); ++i)
        if (!(bounds[i] > bounds[i - 1])) throw std::invalid_argument("histogram " + name + ": bounds not increasing");
    histograms_.push_back(std::make_unique<Histogram>(bounds));
    Series s{name, help, label_text, Type::Histogram};
    s.histogram = histograms_.back().get();
    add(std::move(s));
    return *histograms_.back();
}

void Registry::gauge(const std::string &name, const std::string &help, std::function<double()> read,
                     const std::string &label_text) {
    Series s{name, help, label_text, Type::Gauge};
    s.read = std::move(read);
    add(std::move(s));
}

void Registry::counter_fn(const std::string &name, const std::string &help, std::function<double()> read,
                          const std::string &label_text) {
    Series s{name, help, label_text, Type::Counter};
    s.read = std::move(read);
    add(std::move(s));
}

void Registry::expose(const std::string &name, const std::string &help, const Counter &c, const std::string &label_text) {
    Series s{name, help, label_text, Type::Counter};
    s.counter = &c;
    add(std::move(s));
}

void Registry::expose(const std::string &name, const std::string &help, const Histogram &h,
                      const std::string &label_text) {
    Series s{name, help, label_text, Type::Histogram};
    s.histogram = &h;
    add(std::move(s));
}

std::string Registry::render() const {
    std::string out;
    std::vector<std::string> done;                 // families already written
    for (const Series &first : series_) {
        bool seen = false;
        for (const std::string &n : done) seen = seen || n == first.name;
        if (seen) continue;
        done.push_back(first.name);

        const char *type = first.type == Type::Counter ? "counter" : first.type == Type::Gauge ? "gauge" : "histogram";
        out += "# HELP " + first.name + " " + escape_help(first.help) + "\n";
        out += "# TYPE " + first.name + " " + type + "\n";

        for (const Series &s : series_) {
            if (s.name != first.name) continue;
            if (s.type != Type::Histogram) {
                const std::string v = s.counter ? std::to_string(s.counter->value()) : format_value(s.read());
                out += series_name(s.name, s.label_text) + " " + v + "\n";
            } else {
                // Buckets are CUMULATIVE in the format: le="0.01" counts everything
                // at or below 0.01. The +Inf bucket and _count are both the total of
                // the counts read in THIS reading, so they always agree, even while
                // other threads keep observing (D-084).
                const Histogram::Reading r = s.histogram->read();
                const std::vector<double> &b = s.histogram->bounds();
                std::uint64_t running = 0;
                for (std::size_t i = 0; i < b.size(); ++i) {
                    running += r.counts[i];
                    out += series_name(s.name + "_bucket", s.label_text, "le=\"" + format_value(b[i]) + "\"") + " " +
                           std::to_string(running) + "\n";
                }
                running += r.counts.back();
                out += series_name(s.name + "_bucket", s.label_text, "le=\"+Inf\"") + " " + std::to_string(running) + "\n";
                out += series_name(s.name + "_sum", s.label_text) + " " + format_value(r.sum) + "\n";
                out += series_name(s.name + "_count", s.label_text) + " " + std::to_string(running) + "\n";
            }
        }
    }
    return out;
}
