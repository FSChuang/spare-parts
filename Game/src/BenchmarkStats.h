#pragma once

#include <cstddef>
#include <vector>

// CSC 581 performance checkpoint: tiny, pure statistics helpers shared by the benchmark
// executable and its CSV summary generation. Spare Parts-specific (benchmark tooling),
// not a generic Engine abstraction.

/// Arithmetic mean of `values`. `values` must be non-empty.
double Mean(const std::vector<double>& values);

/// Sample variance: sum((x - mean)^2) / (n - 1). `values` must have at least 2 elements
/// (sample variance is undefined for n < 2); callers with a single sample should report
/// variance as 0 rather than calling this.
double SampleVariance(const std::vector<double>& values);

/// sqrt(SampleVariance(values)).
double SampleStdDev(const std::vector<double>& values);
