#include "NetworkState.h"

Engine::PlayerState ToPlayerState(const Engine::GameObject& object)
{
	Engine::Vector2 position = object.GetPosition();
	Engine::Vector2 velocity = object.HasVelocity() ? object.GetVelocity() : Engine::Vector2{ 0.0f, 0.0f };

	return Engine::PlayerState{ object.GetNetworkId(), position.X, position.Y, velocity.X, velocity.Y };
}

void ApplyPlayerState(Engine::GameObject& object, const Engine::PlayerState& state)
{
	object.SetPosition({ state.PositionX, state.PositionY });
	object.SetVelocity({ state.VelocityX, state.VelocityY });
}
