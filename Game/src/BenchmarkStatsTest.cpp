// Focused unit tests for BenchmarkStats.h (CSC 581 performance checkpoint). Pure logic.

#include "BenchmarkStats.h"

#include <cmath>
#include <cstdio>

namespace
{
	int g_Failures = 0;

	void Check(bool condition, const char* name)
	{
		if (condition)
		{
			std::printf("[PASS] %s\n", name);
		}
		else
		{
			std::printf("[FAIL] %s\n", name);
			++g_Failures;
		}
	}

	bool NearlyEqual(double a, double b, double epsilon)
	{
		return std::fabs(a - b) <= epsilon;
	}
}

int main()
{
	Check(Mean({ 2.0, 4.0, 6.0 }) == 4.0, "Mean_ThreeValues_IsArithmeticMean");
	Check(Mean({ 5.0 }) == 5.0, "Mean_SingleValue_IsItself");

	// Sample variance of {2,4,6}: mean=4, sum((x-4)^2) = 4+0+4 = 8, / (3-1) = 4.
	Check(NearlyEqual(SampleVariance({ 2.0, 4.0, 6.0 }), 4.0, 1e-9), "SampleVariance_MatchesHandComputedValue");
	Check(SampleVariance({ 7.0, 7.0, 7.0 }) == 0.0, "SampleVariance_IdenticalValues_IsZero");

	Check(NearlyEqual(SampleStdDev({ 2.0, 4.0, 6.0 }), 2.0, 1e-9), "SampleStdDev_IsSqrtOfSampleVariance");

	std::printf("\n%s\n", g_Failures == 0 ? "All tests passed." : "Some tests FAILED.");
	return g_Failures == 0 ? 0 : 1;
}
