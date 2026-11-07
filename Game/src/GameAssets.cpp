#include "GameAssets.h"

GameAssets LoadGameAssets(Engine::Renderer& renderer, const std::string& directory)
{
	GameAssets assets{};
	for (int variant = 0; variant < Astro::ShipVariantCount; ++variant)
	{
		assets.Ships[variant] = renderer.LoadTexture(directory + "ship_" + std::to_string(variant + 1) + ".png");
	}
	assets.ReplayShip = renderer.LoadTexture(directory + "ship_replay.png");
	assets.Bullet = renderer.LoadTexture(directory + "bullet.png");
	assets.Explosion = renderer.LoadTexture(directory + "explosion.png");
	assets.Wall = renderer.LoadTexture(directory + "wall.png");
	assets.Stars = renderer.LoadTexture(directory + "stars.png");
	return assets;
}
