#include "ArenaMap.h"

#include "ArenaRules.h"

namespace
{
	constexpr float CentreX = Astro::ArenaWidth / 2.0f;
	constexpr float CentreY = Astro::ArenaHeight / 2.0f;

	// The central reactor block.
	constexpr float ReactorSize = 180.0f;

	// The four barriers: two vertical bars either side of the reactor, two horizontal bars above
	// and below it. Offsets are from the arena centre; the long side is the first dimension.
	constexpr float SideBarOffsetX = 480.0f;
	constexpr Engine::Vector2 SideBarSize{ 36.0f, 220.0f };
	constexpr float EndBarOffsetY = 260.0f;
	constexpr Engine::Vector2 EndBarSize{ 280.0f, 36.0f };

	SolidRect CentredRect(float centreX, float centreY, Engine::Vector2 size)
	{
		return { { centreX - size.X / 2.0f, centreY - size.Y / 2.0f }, size };
	}
}

const std::vector<SolidRect>& ArenaWalls()
{
	static const std::vector<SolidRect> walls = []
	{
		const float left = Astro::WallInset;
		const float top = Astro::WallInset;
		const float width = Astro::ArenaWidth - 2.0f * Astro::WallInset;
		const float height = Astro::ArenaHeight - 2.0f * Astro::WallInset;
		const float t = Astro::WallThickness;
		return std::vector<SolidRect>{
			{ { left, top }, { width, t } },                       // top
			{ { left, top + height - t }, { width, t } },          // bottom
			{ { left, top }, { t, height } },                      // left
			{ { left + width - t, top }, { t, height } },          // right
		};
	}();
	return walls;
}

const std::vector<SolidRect>& ArenaObstacles()
{
	static const std::vector<SolidRect> obstacles = {
		CentredRect(CentreX, CentreY, { ReactorSize, ReactorSize }),
		CentredRect(CentreX - SideBarOffsetX, CentreY, SideBarSize),
		CentredRect(CentreX + SideBarOffsetX, CentreY, SideBarSize),
		CentredRect(CentreX, CentreY - EndBarOffsetY, EndBarSize),
		CentredRect(CentreX, CentreY + EndBarOffsetY, EndBarSize),
	};
	return obstacles;
}

SolidRect ArenaPlayfield()
{
	const float inner = Astro::WallInset + Astro::WallThickness;
	return { { inner, inner }, { Astro::ArenaWidth - 2.0f * inner, Astro::ArenaHeight - 2.0f * inner } };
}
