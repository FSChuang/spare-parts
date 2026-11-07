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

float SpawnHeadingFor(Engine::Vector2 position)
{
	return position.X < Astro::ArenaWidth / 2.0f ? 0.0f : Pi;
}

bool CirclesOverlap(Engine::Vector2 centerA, float radiusA, Engine::Vector2 centerB, float radiusB)
{
	float dx = centerA.X - centerB.X;
	float dy = centerA.Y - centerB.Y;
	float radiusSum = radiusA + radiusB;
	return dx * dx + dy * dy < radiusSum * radiusSum;
}

bool CircleVsRect(Engine::Vector2 center, float radius, Engine::Vector2 rectPosition, Engine::Vector2 rectSize)
{
	// The point of the rectangle nearest the centre (the centre itself if it is inside).
	float nearestX = std::fmax(rectPosition.X, std::fmin(center.X, rectPosition.X + rectSize.X));
	float nearestY = std::fmax(rectPosition.Y, std::fmin(center.Y, rectPosition.Y + rectSize.Y));
	float dx = center.X - nearestX;
	float dy = center.Y - nearestY;

	// Centre inside the rectangle: distance 0, so it overlaps for any positive radius.
	return dx * dx + dy * dy < radius * radius;
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
