#include "ReplayController.h"

#include "ArenaRules.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace
{
	// Control events must beat same-instant gameplay events, so a Start takes effect before
	// the input of the very frame it was pressed in.
	constexpr Engine::EventPriority ControlEventPriority = Engine::EventPriority::Critical;

	void Say(const char* message)
	{
		std::printf("[Replay] %s\n", message);
		std::fflush(stdout);
	}
}

ReplayController::ReplayController(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
                                   const BulletField& bullets, ShipController& ship, PeerEventBridge& bridge,
                                   Engine::ObjectId shipId, std::function<double()> timeSource)
	: m_Events(events),
	  m_Objects(objects),
	  m_Bullets(bullets),
	  m_Ship(ship),
	  m_Bridge(bridge),
	  m_ShipId(shipId),
	  m_TimeSource(timeSource),
	  // Only the LOCAL ship's events belong in the local ship's replay: not remote players'
	  // deaths or spawns, not anything about other objects.
	  m_Recorder(events, std::move(timeSource),
	             [this](const Engine::Event& event)
	             {
		             switch (event.Type)
		             {
		             case Engine::EventType::Input:
			             return true;
		             case Engine::EventType::Collision:
		             {
			             const auto& collision = std::get<Engine::CollisionEventData>(event.Payload);
			             return collision.A == m_ShipId || collision.B == m_ShipId;
		             }
		             case Engine::EventType::Death:
			             return std::get<Engine::DeathEventData>(event.Payload).Subject == m_ShipId;
		             case Engine::EventType::Spawn:
			             return std::get<Engine::SpawnEventData>(event.Payload).Subject == m_ShipId;
		             default:
			             return false;
		             }
	             })
{
	m_Subscriptions.emplace_back(m_Events, Engine::EventType::ReplayStart, [this](const Engine::Event&) { OnStart(); });
	m_Subscriptions.emplace_back(m_Events, Engine::EventType::ReplayStop, [this](const Engine::Event&) { OnStop(); });
	m_Subscriptions.emplace_back(m_Events, Engine::EventType::ReplayPlay, [this](const Engine::Event&) { OnPlay(); });
}

void ReplayController::RequestStartRecording()
{
	m_Events.Raise(Engine::ReplayStartEventData{}, ControlEventPriority);
}

void ReplayController::RequestStopRecording()
{
	m_Events.Raise(Engine::ReplayStopEventData{}, ControlEventPriority);
}

void ReplayController::RequestPlayback()
{
	// See "PLAY ONLY WHILE ALIVE" in the header: this must be decided before the event exists.
	if (!m_Ship.IsAlive() && !m_PlaybackActive)
	{
		Say("playback unavailable while local ship is dead");
		return;
	}

	m_Events.Raise(Engine::ReplayPlayEventData{}, ControlEventPriority);
}

bool ReplayController::IsRecording() const
{
	return m_Recorder.IsRecording();
}

bool ReplayController::IsPlaying() const
{
	return m_PlaybackActive;
}

const std::vector<ReplayFrame>& ReplayController::Frames() const
{
	return m_Frames;
}

const std::vector<Engine::RecordedEvent>& ReplayController::RecordedEvents() const
{
	return m_Recorder.RecordedEvents();
}

const ReplaySample& ReplayController::CurrentSample() const
{
	return m_Frames[m_CurrentFrame].Sample;
}

const ShipSnapshot& ReplayController::SavedLiveShip() const
{
	return m_SavedLive;
}

void ReplayController::OnStart()
{
	// The recorder has already run: if it is recording, the command was accepted (a Start
	// during playback is ignored). Either way a fresh recording means a fresh frame list.
	if (m_Recorder.IsRecording())
	{
		m_Frames.clear();
		m_WasRecording = true;
		Say("recording started");
	}
}

void ReplayController::OnStop()
{
	ReportRecordingFinishedIfNeeded();
}

void ReplayController::OnPlay()
{
	// A Play that arrives mid-recording stops that recording first (the recorder did so).
	ReportRecordingFinishedIfNeeded();

	if (m_PlaybackActive)
	{
		return; // already playing: the recorder ignored this, so do we
	}
	if (!m_Recorder.IsPlaying())
	{
		Say("nothing recorded to play");
		return;
	}

	BeginPlayback();
}

void ReplayController::ReportRecordingFinishedIfNeeded()
{
	if (m_WasRecording && !m_Recorder.IsRecording())
	{
		m_WasRecording = false;
		std::printf("[Replay] recording stopped: %zu frames, %zu events\n", m_Frames.size(),
		            m_Recorder.RecordedEvents().size());
		std::fflush(stdout);
	}
}

void ReplayController::BeginPlayback()
{
	if (m_Frames.empty())
	{
		// The recorder accepted a span with no frames to show; there is nothing scheduled
		// to cancel (nothing was recorded), so simply stand down.
		m_Recorder.EndPlayback();
		Say("nothing recorded to play");
		return;
	}

	// Remember the real game, then hand the ship over to the recording.
	m_SavedLive = m_Ship.Capture();
	m_CurrentFrame = 0;
	m_PlaybackActive = true;
	m_Ship.SetPlaybackMode(true);
	m_Bridge.SetAnnounceEnabled(false);
	m_Bridge.SetRemoteShootsDiscarded(true);

	Say("playback started");
}

void ReplayController::FinishPlayback()
{
	// Order: put the state back first, then reopen the gates.
	m_Ship.Restore(m_SavedLive);
	m_Ship.SetPlaybackMode(false);
	m_Bridge.SetAnnounceEnabled(true);
	m_Bridge.SetRemoteShootsDiscarded(false);
	m_PlaybackActive = false;

	Say("playback complete");
}

void ReplayController::CaptureFrame()
{
	if (!m_Recorder.IsRecording())
	{
		return;
	}

	double relativeTime = std::max(0.0, m_TimeSource() - m_Recorder.RecordingStartTime());
	m_Frames.push_back(ReplayFrame{ relativeTime, Sample() });
}

void ReplayController::UpdatePlayback()
{
	if (!m_PlaybackActive)
	{
		return;
	}

	// The frame in force is the latest one at or before the elapsed playback time. Stepping
	// (not interpolating) means a playback update of ANY length — longer than the recorded
	// ones, shorter, or zero — shows only states that were really recorded, in order, and
	// never alters the recorded trajectory.
	double elapsed = m_TimeSource() - m_Recorder.PlaybackStartTime();
	while (m_CurrentFrame + 1 < m_Frames.size() && m_Frames[m_CurrentFrame + 1].RelativeTime <= elapsed)
	{
		++m_CurrentFrame;
	}
	Show(m_Frames[m_CurrentFrame].Sample);

	if (m_Recorder.PollPlaybackComplete())
	{
		FinishPlayback();
	}
}

ReplaySample ReplayController::Sample() const
{
	const Engine::GameObject& ship = *m_Objects.Find(m_ShipId);

	ReplaySample sample;
	sample.ShipCentre = CenterOf(ship);
	sample.ShipVelocity = ship.GetVelocity();
	sample.HeadingRadians = m_Ship.Heading();
	sample.Alive = m_Ship.IsAlive();
	sample.Invulnerable = m_Ship.IsInvulnerable();

	// Only this ship's own bullets: those rebuilt from other players' Shoots are the live
	// timeline's, not part of this player's replay.
	for (const Bullet& bullet : m_Bullets.All())
	{
		const Engine::GameObject* object = m_Objects.Find(bullet.Id);
		if (bullet.Owner == m_ShipId && object != nullptr)
		{
			sample.Bullets.push_back({ CenterOf(*object) });
		}
	}
	return sample;
}

void ReplayController::Show(const ReplaySample& sample)
{
	// Presentation of the recorded ship. Nothing here is ever published (Game publishes the
	// saved live state instead), and Restore() puts the real state back when playback ends.
	m_Ship.ApplyReplayPose(sample.ShipCentre, sample.ShipVelocity, sample.HeadingRadians, sample.Alive);
}
