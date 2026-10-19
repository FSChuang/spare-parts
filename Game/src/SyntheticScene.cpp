#include "SyntheticScene.h"

namespace
{
	// Deterministic per-index velocity variation for moving objects: a small, bounded,
	// repeating pattern (no randomness) so different objects move at genuinely different
	// speeds/directions without needing random_device.
	constexpr float BaseSpeed = 50.0f;      // px/s
	constexpr float SpeedVariationStep = 5.0f;
	constexpr int SpeedVariationBuckets = 7;
	constexpr float VerticalVariationStep = 10.0f;
	constexpr int VerticalVariationBuckets = 3;

	constexpr float ObjectSpacing = 10.0f; // purely synthetic, not a rendered world
	constexpr float MovingObjectBaselineY = 100.0f;
}

std::vector<Engine::ExchangeObjectState> BuildInitialScene(int staticCount, int movingCount)
{
	std::vector<Engine::ExchangeObjectState> scene;
	scene.reserve(static_cast<std::size_t>(staticCount) + static_cast<std::size_t>(movingCount));

	std::uint32_t nextId = 1;

	for (int i = 0; i < staticCount; ++i)
	{
		scene.push_back(Engine::ExchangeObjectState{ nextId, static_cast<float>(i) * ObjectSpacing, 0.0f, 0.0f, 0.0f });
		++nextId;
	}

	for (int i = 0; i < movingCount; ++i)
	{
		float velocityX = BaseSpeed + static_cast<float>(i % SpeedVariationBuckets) * SpeedVariationStep;
		float velocityY = static_cast<float>((i % VerticalVariationBuckets) - 1) * VerticalVariationStep;
		scene.push_back(Engine::ExchangeObjectState{ nextId, static_cast<float>(i) * ObjectSpacing,
			                                           MovingObjectBaselineY, velocityX, velocityY });
		++nextId;
	}

	return scene;
}

void AdvanceMovingObjects(std::vector<Engine::ExchangeObjectState>& scene, int movingCount, float deltaTime)
{
	std::size_t firstMovingIndex = scene.size() - static_cast<std::size_t>(movingCount);
	for (std::size_t i = firstMovingIndex; i < scene.size(); ++i)
	{
		scene[i].PositionX += scene[i].VelocityX * deltaTime;
		scene[i].PositionY += scene[i].VelocityY * deltaTime;
	}
}
