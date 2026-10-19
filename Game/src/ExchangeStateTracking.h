#pragma once

#include "Engine/Network/ExchangeProtocol.h"

#include <unordered_map>
#include <vector>

// CSC 581 performance checkpoint: the sender-side change-detection and receiver-side
// state-application logic shared by both NetworkExchangeCorrectnessTest.cpp (in-process,
// no sockets) and NetworkPerformanceBenchmark.cpp (real ZeroMQ sockets) — the same code
// that gets benchmarked is the code whose correctness is verified, not a parallel
// reimplementation. Spare Parts-specific (benchmark tooling), not a generic Engine
// abstraction; Engine::ExchangeFrame/ExchangeObjectState are the only Engine types used.

/// Sender-side bookkeeping for the Delta-State strategy: remembers the last state sent
/// for each object id and reports which objects changed since then. An id never seen
/// before always counts as changed — this is what makes the very first call naturally
/// report every object (the complete initial state), with no special-cased "first call"
/// branch anywhere. The Full-State strategy does not use this at all; it always sends
/// every object, every call, regardless of what changed.
class ExchangeSenderState
{
public:
	/// Returns the subset of `current` that differs from (or has no entry in) what was
	/// last reported changed, then updates its own bookkeeping to `current`'s values for
	/// exactly the objects returned.
	std::vector<Engine::ExchangeObjectState> ComputeChanged(const std::vector<Engine::ExchangeObjectState>& current);

private:
	std::unordered_map<std::uint32_t, Engine::ExchangeObjectState> m_LastSent;
};

/// Receiver-side tracked object state, built by applying a sequence of ExchangeFrames.
/// A FullState frame REPLACES the entire tracked set; a DeltaState frame MERGES its
/// records into whatever is already tracked, leaving every other tracked object
/// untouched — this is the one behavioral difference between the two strategies on the
/// receiving end.
class ExchangeReceiverState
{
public:
	void Apply(const Engine::ExchangeFrame& frame);

	const std::unordered_map<std::uint32_t, Engine::ExchangeObjectState>& Objects() const;

private:
	std::unordered_map<std::uint32_t, Engine::ExchangeObjectState> m_Objects;
};
