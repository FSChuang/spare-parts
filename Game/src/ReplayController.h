#pragma once

#include "Engine/Core/Core.h"
#include "Engine/Event/EventManager.h"
#include "Engine/Event/ReplayRecorder.h"
#include "Engine/GameObject/GameObject.h"
#include "Engine/GameObject/GameObjectRegistry.h"

#include "BulletField.h"
#include "PeerEventBridge.h"
#include "ShipController.h"

#include <functional>
#include <vector>

// Replay for the Astro arena (CSC 581). Spare Parts-specific, SDL-free and socket-free so it is
// unit tested without a Game. A LOCAL review of the local ship's own play: in memory, never
// networked, never authoritative.
//
// HYBRID DESIGN. A replay needs two things, and neither is enough alone:
//
//  1. EVENTS — Engine::ReplayRecorder records the local ship's Input / Death / Spawn (and
//     Collision) events and, on playback, re-schedules them through the SAME EventManager.
//     Recording and playback are driven by three ordinary control events (ReplayStart /
//     ReplayStop / ReplayPlay), which Game raises from F5 / F6 / F7.
//  2. STATE — one ReplayFrame per game update. Replaying inputs alone cannot reproduce what
//     happened: frame times vary (including zero-length frames) and bullet flight is integrated
//     with that frame time. So the recorded samples, not a re-simulation, own what the replayed
//     ship and its bullets visibly do.
//
// BULLETS in a replay are just positions in a sample (a ReplayBullet), drawn by Game. They are
// never GameObjects, never put in the live BulletField, and never moved or aged during playback.
// Only the local ship's own bullets are sampled (those whose owner is the local ship); bullets
// rebuilt from other players' Shoots are not part of this player's replay.
//
// PLAYBACK is a review, not time travel. When it starts, the live ship state it will overwrite is
// captured; each update shows the recorded frame for the elapsed playback time; when the recorded
// span has elapsed the captured state is put back and live play resumes exactly where it was.
// While playing:
//   - the ship controller is in playback mode: live input, steering, firing, wall and bullet
//     hit detection do nothing, and replayed events are observed but never acted on;
//   - Game skips physics and does not touch the live BulletField at all: its bullets neither move,
//     age, expire nor collide, and resume from the very same state afterwards;
//   - a replayed Death, Spawn or Shoot is never announced to peers (announce gate), and Game
//     publishes the SAVED live ship state, so other clients never see the replay as movement;
//   - a remote Shoot that arrives is drained and discarded (not rebuilt, not queued for after).
//     Remote players' state, deaths and spawns keep applying — they are live.
//
// TIMERS. Fire cooldown and invulnerability are captured as REMAINING durations and rebased onto
// the logical time at which playback ends. Playback consumes logical time, so putting back the
// old absolute timestamps would have lost them.
//
// PLAY ONLY WHILE ALIVE. Playback starts only if the live ship is alive; otherwise it is refused
// with a message and the recording stays intact. A dead live ship has a Spawn already scheduled
// in the EventManager, which cannot be cancelled: playback mode makes the ship controller ignore
// Spawn events, so it would swallow that real respawn and strand the ship dead. (A recording that
// itself contains a death and respawn plays back fine while the live ship is alive.) The check is
// made when Play is requested, before the control event is raised: the recorder schedules the
// playback events the moment it sees the event, so refusing afterwards would be too late. Nothing
// can kill the ship between the request and its dispatch in the same frame.
//
// Which handler runs first matters: this class subscribes to the replay control events AFTER its
// ReplayRecorder does, and EventManager runs a type's handlers in registration order. So by the
// time these handlers run, the recorder has already accepted or ignored the command, and they
// simply look at its state.
//
// Not recorded: replay events themselves, events about remote players (the recorder filter keeps
// only the local ship's), anything during playback, remote ships. Not implemented (out of
// scope): files, seeking, reverse play, cancelling playback.

/// A bullet as the replay shows it: a centre position and nothing else.
struct ReplayBullet
{
	Engine::Vector2 Centre;
};

/// What a replay needs to show of the local ship and its bullets at one instant.
struct ReplaySample
{
	Engine::Vector2 ShipCentre;
	Engine::Vector2 ShipVelocity;
	float HeadingRadians;
	bool Alive;
	/// Whether the ship was blinking as invulnerable (presentation only).
	bool Invulnerable;
	std::vector<ReplayBullet> Bullets;
};

/// One recorded game update: its time since the recording began (logical time — never wall
/// clock) and the state at the end of that update.
struct ReplayFrame
{
	double RelativeTime;
	ReplaySample Sample;
};

class ReplayController
{
public:
	/// All references must outlive this controller (Game declares it last). `timeSource` must be
	/// the logical clock the EventManager uses.
	ReplayController(Engine::EventManager& events, Engine::GameObjectRegistry& objects, const BulletField& bullets,
	                 ShipController& ship, PeerEventBridge& bridge, Engine::ObjectId shipId,
	                 std::function<double()> timeSource);

	// Handlers capture `this`; never copy or move.
	ReplayController(const ReplayController&) = delete;
	ReplayController& operator=(const ReplayController&) = delete;

	/// Raise the corresponding replay control event (they take effect when dispatched).
	/// RequestPlayback raises nothing, and says so, while the live ship is dead.
	void RequestStartRecording();
	void RequestStopRecording();
	void RequestPlayback();

	/// Call once at the END of every game update: stores a ReplayFrame while recording.
	void CaptureFrame();

	/// Call once per game update, after dispatching: while playing, shows the recorded frame for
	/// the elapsed playback time and, once the whole recorded span has elapsed, puts the live
	/// state back and ends playback.
	void UpdatePlayback();

	bool IsRecording() const;
	/// True from when playback is accepted until it completes. Game skips live simulation while
	/// this is true.
	bool IsPlaying() const;

	const std::vector<ReplayFrame>& Frames() const;
	const std::vector<Engine::RecordedEvent>& RecordedEvents() const;

	/// The frame being shown; valid only while IsPlaying(). Game draws the replayed bullets and
	/// the invulnerability blink from it.
	const ReplaySample& CurrentSample() const;

	/// The live ship state captured when playback began; valid only while IsPlaying(). Game
	/// publishes this over the network instead of the replayed ship.
	const ShipSnapshot& SavedLiveShip() const;

private:
	void OnStart();
	void OnStop();
	void OnPlay();

	void BeginPlayback();
	void FinishPlayback();
	void ReportRecordingFinishedIfNeeded();

	ReplaySample Sample() const;
	void Show(const ReplaySample& sample);

	Engine::EventManager& m_Events;
	Engine::GameObjectRegistry& m_Objects;
	const BulletField& m_Bullets;
	ShipController& m_Ship;
	PeerEventBridge& m_Bridge;
	Engine::ObjectId m_ShipId;
	std::function<double()> m_TimeSource;

	// Constructed before the subscriptions below, so its control-event handlers run first.
	Engine::ReplayRecorder m_Recorder;

	std::vector<ReplayFrame> m_Frames;
	bool m_WasRecording = false;

	bool m_PlaybackActive = false;
	std::size_t m_CurrentFrame = 0;
	ShipSnapshot m_SavedLive{};

	// Declared last: destroyed first, unsubscribing while m_Events is still alive.
	std::vector<Engine::ScopedSubscription> m_Subscriptions;
};
