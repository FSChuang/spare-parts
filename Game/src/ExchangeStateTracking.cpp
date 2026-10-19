#include "ExchangeStateTracking.h"

namespace
{
	bool StatesEqual(const Engine::ExchangeObjectState& a, const Engine::ExchangeObjectState& b)
	{
		return a.Id == b.Id && a.PositionX == b.PositionX && a.PositionY == b.PositionY && a.VelocityX == b.VelocityX &&
		       a.VelocityY == b.VelocityY;
	}
}

std::vector<Engine::ExchangeObjectState> ExchangeSenderState::ComputeChanged(
    const std::vector<Engine::ExchangeObjectState>& current)
{
	std::vector<Engine::ExchangeObjectState> changed;
	for (const Engine::ExchangeObjectState& object : current)
	{
		auto it = m_LastSent.find(object.Id);
		if (it != m_LastSent.end() && StatesEqual(it->second, object))
		{
			continue;
		}

		changed.push_back(object);
		m_LastSent[object.Id] = object;
	}
	return changed;
}

void ExchangeReceiverState::Apply(const Engine::ExchangeFrame& frame)
{
	if (frame.Strategy == Engine::ExchangeStrategy::FullState)
	{
		m_Objects.clear();
	}

	for (const Engine::ExchangeObjectState& object : frame.Objects)
	{
		m_Objects[object.Id] = object;
	}
}

const std::unordered_map<std::uint32_t, Engine::ExchangeObjectState>& ExchangeReceiverState::Objects() const
{
	return m_Objects;
}
