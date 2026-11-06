#include "ReplayController.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace
{
	// Control events must beat same-instant gameplay events, so a Start takes effect before
	// the input of the very frame it was pressed in.
	constexpr Engine::EventPriority ControlEventPriority = Engine::EventPriority::Critical;
}

ReplayController::ReplayController(Engine::EventManager& events, Engine::GameObjectRegistry& objects,
                                   LocalPlayerController& player, PeerEventBridge& bridge, ReplayObjects ids,
                                   std::function<double()> timeSource)
	: m_Events(events),
	  m_Objects(objects),
	  m_Player(player),
	  m_Bridge(bridge),
	  m_Ids(ids),
	  m_TimeSource(timeSource),
	  // Only the LOCAL player's events belong in the local player's replay: not remote
	  // players' deaths, not anything about other objects.
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
			             return collision.A == m_Ids.Player || collision.B == m_Ids.Player;
		             }
		             case Engine::EventType::Death:
			             return std::get<Engine::DeathEventData>(event.Payload).Subject == m_Ids.Player;
		             case Engine::EventType::Spawn:
			             return std::get<Engine::SpawnEventData>(event.Payload).Subject == m_Ids.Player;
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

const GameplaySample& ReplayController::SavedLiveSample() const
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
		std::printf("[Replay] recording started\n");
		std::fflush(stdout);
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
		std::printf("[Replay] nothing recorded to play\n");
		std::fflush(stdout);
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
		std::printf("[Replay] nothing recorded to play\n");
		std::fflush(stdout);
		return;
	}

	// Remember the real game, then hand the player over to the recording.
	m_SavedLive = Capture();
	m_CurrentFrame = 0;
	m_PlaybackActive = true;
	m_Player.SetPlaybackMode(true);
	m_Bridge.SetAnnounceEnabled(false);

	std::printf("[Replay] playback started\n");
	std::fflush(stdout);
}

void ReplayController::FinishPlayback()
{
	Apply(m_SavedLive);
	m_Player.SetPlaybackMode(false);
	m_Bridge.SetAnnounceEnabled(true);
	m_PlaybackActive = false;

	std::printf("[Replay] playback complete\n");
	std::fflush(stdout);
}

void ReplayController::CaptureFrame()
{
	if (!m_Recorder.IsRecording())
	{
		return;
	}

	double relativeTime = std::max(0.0, m_TimeSource() - m_Recorder.RecordingStartTime());
	m_Frames.push_back(ReplayFrame{ relativeTime, Capture() });
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
	Apply(m_Frames[m_CurrentFrame].Sample);

	if (m_Recorder.PollPlaybackComplete())
	{
		FinishPlayback();
	}
}

GameplaySample ReplayController::Capture() const
{
	const Engine::GameObject& player = *m_Objects.Find(m_Ids.Player);
	return GameplaySample{ player.GetPosition(),
	                       player.GetVelocity(),
	                       m_Player.IsGrounded(),
	                       m_Player.CameraOffsetX(),
	                       m_Objects.Find(m_Ids.HorizontalPlatform)->GetPosition(),
	                       m_Objects.Find(m_Ids.VerticalPlatform)->GetPosition(),
	                       m_Objects.Find(m_Ids.Enemy)->GetPosition() };
}

void ReplayController::Apply(const GameplaySample& sample)
{
	Engine::GameObject& player = *m_Objects.Find(m_Ids.Player);
	player.SetPosition(sample.PlayerPosition);
	player.SetVelocity(sample.PlayerVelocity);
	m_Player.SetGrounded(sample.Grounded);
	m_Player.SetCameraOffsetX(sample.CameraOffsetX);

	// Local presentation only: nothing here is ever published, and live networking
	// overwrites the platforms again as soon as playback ends.
	m_Objects.Find(m_Ids.HorizontalPlatform)->SetPosition(sample.HorizontalPlatformPosition);
	m_Objects.Find(m_Ids.VerticalPlatform)->SetPosition(sample.VerticalPlatformPosition);
	m_Objects.Find(m_Ids.Enemy)->SetPosition(sample.EnemyPosition);
}
