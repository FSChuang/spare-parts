#include "ArenaRules.h"

#include <cmath>

namespace
{
	constexpr float Pi = 3.14159265358979323846f;
	constexpr float TwoPi = 2.0f * Pi;
}

Engine::Vector2 ForwardOf(float headingRadians)
{
	return { std::cos(headingRadians), std::sin(headingRadians) };
}

float NormalizeHeading(float headingRadians)
{
	float folded = std::fmod(headingRadians, TwoPi);
	if (folded >= Pi)
	{
		folded -= TwoPi;
	}
	else if (folded < -Pi)
	{
		folded += TwoPi;
	}
	return folded;
}

float HeadingTowardArenaCentre(Engine::Vector2 position)
{
	return std::atan2(Astro::ArenaHeight / 2.0f - position.Y, Astro::ArenaWidth / 2.0f - position.X);
}

bool CirclesOverlap(Engine::Vector2 centerA, float radiusA, Engine::Vector2 centerB, float radiusB)
{
	float dx = centerA.X - centerB.X;
	float dy = centerA.Y - centerB.Y;
	float radiusSum = radiusA + radiusB;
	return dx * dx + dy * dy < radiusSum * radiusSum;
}

Engine::Vector2 WrapShipCentre(Engine::Vector2 center, float radius)
{
	const float wrapWidth = Astro::ArenaWidth + 2.0f * radius;
	const float wrapHeight = Astro::ArenaHeight + 2.0f * radius;

	if (center.X < -radius)
	{
		center.X += wrapWidth;
	}
	else if (center.X > Astro::ArenaWidth + radius)
	{
		center.X -= wrapWidth;
	}

	if (center.Y < -radius)
	{
		center.Y += wrapHeight;
	}
	else if (center.Y > Astro::ArenaHeight + radius)
	{
		center.Y -= wrapHeight;
	}

	return center;
}

bool IsFullyOutsideArena(Engine::Vector2 center, float radius)
{
	return center.X < -radius || center.X > Astro::ArenaWidth + radius || center.Y < -radius ||
	       center.Y > Astro::ArenaHeight + radius;
}

Engine::Vector2 CenterOf(const Engine::GameObject& object)
{
	Engine::Vector2 position = object.GetPosition();
	Engine::Vector2 size = object.GetSize();
	return { position.X + size.X / 2.0f, position.Y + size.Y / 2.0f };
}

void SetCenter(Engine::GameObject& object, Engine::Vector2 center)
{
	Engine::Vector2 size = object.GetSize();
	object.SetPosition({ center.X - size.X / 2.0f, center.Y - size.Y / 2.0f });
}
