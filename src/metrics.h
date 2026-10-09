#ifndef METRICS_H
#define METRICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Metrics, in the Prometheus text exposition format (decisions D-083, D-084).
//
// Three instruments:
//   Counter    a number that only goes up: requests served, evictions, bytes sent
//   Histogram  a counter per bucket plus a sum and a count: latencies, hop counts
//   gauge      a level read when the scrape arrives: chunks held, the Raft term
//
// A Registry holds every series a process exposes and renders them as text.
// Nothing here opens a socket (http.h serves the text), so the tests render and
// check it in process.
//
// The concurrency rule: every series is REGISTERED before the thread that serves
// /metrics starts, and the registry's lists are only read after that -- the same
// "all writes before the first reader exists" rule as node.cpp's settings. Counter
// and Histogram values change all the time, from any thread, through atomics.
// ---------------------------------------------------------------------------

// A number that only goes up.
//
// std::memory_order_relaxed: the increment is indivisible -- two threads adding 1
// at once always make 2 -- but it orders nothing else. That is all a counter
// needs, because no thread reads some OTHER data on the strength of having seen a
// counter change.
class Counter {
public:
    void add(std::uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

// A distribution: how many observations fell at or below each bound, their sum
// and their count. Bounds are fixed when it is made (D-084).
class Histogram {
public:
    // `bounds` must be strictly increasing and non-empty.
    explicit Histogram(std::vector<double> bounds);

    // One observation, in the metric's base unit (seconds, hops). Counted in the
    // first bucket whose bound is >= x, or above every bound.
    void observe(double x);

    // One reading of every bucket, for rendering. counts[i] is the number of
    // observations in bucket i alone (not cumulative); counts.back() is the
    // overflow, above every bound. `sum` is in the base unit.
    struct Reading {
        std::vector<std::uint64_t> counts;
        double sum = 0;
    };
    Reading read() const;

    const std::vector<double> &bounds() const { return bounds_; }

private:
    std::vector<double> bounds_;
    // One atomic per bucket, plus one for the overflow. std::atomic can be neither
    // copied nor moved, so it cannot live in a std::vector that might reallocate;
    // an array made once, at the right size, never moves.
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts_;
    // The sum in millionths of the unit: an integer, so it can be one atomic
    // add (a std::atomic<double> has no fetch_add before C++20). 1 us resolution.
    std::atomic<std::uint64_t> sum_millionths_{0};
};

// Latency buckets, in seconds: 0.5 ms to 10 s in a 1-2-5 series.
const std::vector<double> &latency_buckets();
// Hop buckets: 0 to 10 one at a time, then 12, 16, 24, 32, 64.
const std::vector<double> &hop_buckets();

// Label text for a series: {{"verb","get"},{"result","ok"}} -> verb="get",result="ok".
// Values are escaped as the format requires (backslash, double quote, newline).
std::string labels(std::initializer_list<std::pair<std::string, std::string>> pairs);

// A metric name the format accepts: [a-zA-Z_:][a-zA-Z0-9_:]*.
bool valid_metric_name(const std::string &name);

// A number as the format writes it: integers without a decimal point, NaN as
// "NaN", infinities as "+Inf" / "-Inf", anything else to 15 significant digits.
std::string format_value(double v);

class Registry {
public:
    // Register a series and return it. Several series may share a name if their
    // labels differ: they are rendered as one family, under one HELP and TYPE.
    // Throws std::invalid_argument for an invalid name, or for one name registered
    // as two different types -- both are programming errors, found at start-up.
    Counter &counter(const std::string &name, const std::string &help, const std::string &label_text = "");
    Histogram &histogram(const std::string &name, const std::string &help, const std::vector<double> &bounds,
                         const std::string &label_text = "");
    // A gauge is a function, called during each render, on the thread that
    // renders. It must not block for long, and must not call render().
    void gauge(const std::string &name, const std::string &help, std::function<double()> read,
               const std::string &label_text = "");

    // Series whose values live elsewhere -- in a module's own atomics or
    // instruments, which the module updates without knowing about the registry.
    // The object must outlive every render.
    void counter_fn(const std::string &name, const std::string &help, std::function<double()> read,
                    const std::string &label_text = "");
    void expose(const std::string &name, const std::string &help, const Counter &c, const std::string &label_text = "");
    void expose(const std::string &name, const std::string &help, const Histogram &h, const std::string &label_text = "");

    // The whole exposition: for each family in the order first registered,
    // "# HELP", "# TYPE", then every series of that name.
    std::string render() const;

private:
    enum class Type { Counter, Gauge, Histogram };
    struct Series {
        std::string name, help, label_text;
        Type type;
        const Counter *counter = nullptr;          // a counter, or
        const Histogram *histogram = nullptr;      // a histogram, or
        std::function<double()> read;              // a value read at render: a gauge, or a counter kept elsewhere
    };
    void add(Series s);

    std::vector<Series> series_;
    std::vector<std::unique_ptr<Counter>> counters_;       // owned here; addresses never change
    std::vector<std::unique_ptr<Histogram>> histograms_;
};

#endif
