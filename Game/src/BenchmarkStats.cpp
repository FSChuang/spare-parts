#include "BenchmarkStats.h"

#include <cmath>

double Mean(const std::vector<double>& values)
{
	double sum = 0.0;
	for (double value : values)
	{
		sum += value;
	}
	return sum / static_cast<double>(values.size());
}

double SampleVariance(const std::vector<double>& values)
{
	double mean = Mean(values);
	double sumSquaredDeviation = 0.0;
	for (double value : values)
	{
		double deviation = value - mean;
		sumSquaredDeviation += deviation * deviation;
	}
	return sumSquaredDeviation / static_cast<double>(values.size() - 1);
}

double SampleStdDev(const std::vector<double>& values)
{
	return std::sqrt(SampleVariance(values));
}
