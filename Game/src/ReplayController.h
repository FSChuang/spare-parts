#pragma once

#include "Engine/Core/Core.h"
#include "Engine/Event/EventManager.h"
#include "Engine/Event/ReplayRecorder.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"

#include "LocalPlayerController.h"
#include "PeerEventBridge.h"

#include <functional>
#include <vector>

// Milestone 4 Checkpoint 4: Spare Parts' replay (CSC 581). Spare Parts-specific, SDL-free and
// socket-free so it is unit tested without a Game.
//
// HYBRID DESIGN. A replay needs two things, and neither is enough alone:
//
//  1. EVENTS — Engine::ReplayRecorder records the local player's Input/Death/Spawn (and
//     Collision) events and, on playback, re-schedules them through the SAME EventManager.
//     Recording and playback are driven by three ordinary control events
//     (ReplayStart / ReplayStop / ReplayPlay), which Game raises from F5 / F6 / F7.
//  2. STATE — one ReplayFrame per game update (below). Replaying inputs alone cannot
//     reproduce what happened: frame times vary (including zero-length frames), the moving
//     platforms are driven by the server, and the enemy by this client's own clock. So the
//     recorded samples, not a re-simulation, own what the replayed player visibly does.
//
// PLAYBACK is a local review, not time travel. When it starts, the live state it will
// overwrite is saved; each update applies the recorded frame matching the elapsed playback
// time; when the recorded span has elapsed the saved state is restored and live play resumes
// exactly where it was. While playing:
//   - the player controller is in playback mode: live input, movement, landing and hazard
//     detection do nothing, and replayed events are observed but never acted on;
//   - Game skips physics, enemy patrol and scrolling (the recorded frames own all three);
//   - a replayed Death is not announced to peers, and Game publishes the SAVED live position,
//     so other clients never see the replay as movement.
//
// Which handler runs first matters: this class subscribes to the replay control events AFTER
// its ReplayRecorder does, and EventManager runs a type's handlers in registration order. So
// by the time these handlers run, the recorder has already accepted or ignored the command,
// and they simply look at its state.
//
// Not recorded: Replay events themselves, events about remote players (the recorder filter
// keeps only the local player's), anything during playback, and the full GameObjectRegistry —
// only the small sample below. Not implemented (out of scope): files, seeking, reverse play.

/// What a replay needs to reproduce, or restore, of the local view of the game at one instant.
struct GameplaySample
{
	Engine::Vector2 PlayerPosition;
	Engine::Vector2 PlayerVelocity;
	bool Grounded;
	float CameraOffsetX;
	// The two server-driven platforms and the client-local enemy visibly affect what the
	// local player does, so they are sampled too (static platforms never change).
	Engine::Vector2 HorizontalPlatformPosition;
	Engine::Vector2 VerticalPlatformPosition;
	Engine::Vector2 EnemyPosition;
};

/// One recorded game update: its time since the recording began (logical time — never wall
/// clock) and the state at the end of that update.
struct ReplayFrame
{
	double RelativeTime;
	GameplaySample Sample;
};

/// The objects a sample reads and playback writes.
struct ReplayObjects
{
	Engine::ObjectId Player;
	Engine::ObjectId HorizontalPlatform;
	Engine::ObjectId VerticalPlatform;
	Engine::ObjectId Enemy;
};

class ReplayController
{
public:
	/// All references must outlive this controller (Game declares it last). `timeSource` must
	/// be the logical clock the EventManager uses.
	ReplayController(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
	                 LocalPlayerController& player, PeerEventBridge& bridge, ReplayObjects ids,
	                 std::function<double()> timeSource);

	// Handlers capture `this`; never copy or move.
	ReplayController(const ReplayController&) = delete;
	ReplayController& operator=(const ReplayController&) = delete;

	/// Raise the corresponding replay control event (they take effect when dispatched).
	void RequestStartRecording();
	void RequestStopRecording();
	void RequestPlayback();

	/// Call once at the END of every game update: stores a ReplayFrame while recording.
	void CaptureFrame();

	/// Call once per game update, after dispatching: while playing, applies the recorded frame
	/// for the elapsed playback time and, once the whole recorded span has elapsed, restores
	/// the saved live state and ends playback.
	void UpdatePlayback();

	bool IsRecording() const;
	/// True from when playback is accepted until it completes. Game skips live simulation
	/// while this is true.
	bool IsPlaying() const;

	const std::vector<ReplayFrame>& Frames() const;
	const std::vector<Engine::RecordedEvent>& RecordedEvents() const;

	/// The live state saved when playback began; valid only while IsPlaying(). Game publishes
	/// this over the network instead of the replayed player.
	const GameplaySample& SavedLiveSample() const;

private:
	void OnStart();
	void OnStop();
	void OnPlay();

	void BeginPlayback();
	void FinishPlayback();
	void ReportRecordingFinishedIfNeeded();

	GameplaySample Capture() const;
	void Apply(const GameplaySample& sample);

	Engine::EventManager& m_Events;
	Engine::GameObjectRegistry& m_Objects;
	LocalPlayerController& m_Player;
	PeerEventBridge& m_Bridge;
	ReplayObjects m_Ids;
	std::function<double()> m_TimeSource;

	// Constructed before the subscriptions below, so its control-event handlers run first.
	Engine::ReplayRecorder m_Recorder;

	std::vector<ReplayFrame> m_Frames;
	bool m_WasRecording = false;

	bool m_PlaybackActive = false;
	std::size_t m_CurrentFrame = 0;
	GameplaySample m_SavedLive{};

	// Declared last: destroyed first, unsubscribing while m_Events is still alive.
	std::vector<Engine::ScopedSubscription> m_Subscriptions;
};
