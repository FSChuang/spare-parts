// Focused tests for ReplayController (Milestone 4 Checkpoint 4). Uses the REAL
// ReplayController, ReplayRecorder, LocalPlayerController, PeerEventBridge, EventManager and
// GameObjects on a fake logical clock, so what is proven is the real record / playback /
// restore / suppression behavior. Scene::Frame() runs the same sequence of steps as
// Game::Update (events -> live simulation unless replaying -> networking -> playback ->
// capture); the one step it supplies itself is physics integration, using the real
// Engine::PhysicsSystem. No SDL, no sockets.

#include "ReplayController.h"

#include "Engine/Physics/PhysicsSystem.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace
{
	int g_Failures = 0;

	void Check(bool condition, const char* name)
	{
		if (condition)
		{
			std::printf("[PASS] %s\n", name);
		}
		else
		{
			std::printf("[FAIL] %s\n", name);
			++g_Failures;
		}
	}

	constexpr float MoveSpeed = 100.0f;
	constexpr float JumpSpeed = 650.0f;
	constexpr float ViewWidth = 1920.0f;
	constexpr Engine::Vector2 Spawn{ 80.0f, 900.0f };
	constexpr Engine::PlayerId LocalNetworkId = 1;
	constexpr Engine::PlayerId RemoteNetworkId = 7;

	// Exactly representable frame times, so recorded and replayed arithmetic is exact.
	constexpr double DtPattern[] = { 1.0 / 64, 1.0 / 32, 1.0 / 16, 1.0 / 64, 1.0 / 128, 1.0 / 32 };
	double Dt(int frame)
	{
		return DtPattern[frame % 6];
	}

	struct Seen
	{
		Engine::Event Event;
		bool DuringPlayback;
	};

	struct Scene
	{
		Scene()
			: Events([this] { return Now; }), Physics(980.0f)
		{
			Engine::GameObject& player = Objects.Create(Engine::ObjectType::Player, { 100.0f, 100.0f }, { 50.0f, 50.0f });
			player.SetVelocity({ 0.0f, 0.0f });
			player.SetNetworkId(LocalNetworkId);
			PlayerId = player.GetId();

			Objects.Create(Engine::ObjectType::Platform, { 0.0f, 1000.0f }, { 850.0f, 40.0f });
			HorizontalId = Objects.Create(Engine::ObjectType::Platform, { 900.0f, 600.0f }, { 220.0f, 30.0f }).GetId();
			VerticalId = Objects.Create(Engine::ObjectType::Platform, { 2450.0f, 500.0f }, { 150.0f, 30.0f }).GetId();
			EnemyId = Objects.Create(Engine::ObjectType::Generic, { 5000.0f, 0.0f }, { 50.0f, 50.0f }).GetId();
			DeathZoneId = Objects.Create(Engine::ObjectType::DeathZone, { 0.0f, 1080.0f }, { 3840.0f, 1000000.0f }).GetId();
			Engine::GameObject& remote = Objects.Create(Engine::ObjectType::Player, { 2000.0f, 800.0f }, { 50.0f, 50.0f });
			remote.SetNetworkId(RemoteNetworkId);
			RemoteId = remote.GetId();

			Controller = std::make_unique<LocalPlayerController>(
			    Events, Objects, LocalPlayerObjects{ PlayerId, EnemyId, DeathZoneId },
			    LocalPlayerConfig{ MoveSpeed, JumpSpeed, ViewWidth }, [] { return Spawn; });
			Bridge = std::make_unique<PeerEventBridge>(Events, Objects, PlayerId, [this] { return Now; },
			                                          [this](Engine::NetworkEventKind kind) { SentToPeers.push_back(kind); });
			Replay = std::make_unique<ReplayController>(
			    Events, Objects, *Controller, *Bridge, ReplayObjects{ PlayerId, HorizontalId, VerticalId, EnemyId },
			    [this] { return Now; });

			for (Engine::EventType type : { Engine::EventType::Input, Engine::EventType::Death, Engine::EventType::Spawn,
			                                Engine::EventType::Collision })
			{
				Subscriptions.emplace_back(Events, type,
				                           [this](const Engine::Event& event)
				                           { Dispatched.push_back(Seen{ event, Replay->IsPlaying() }); });
			}
		}

		Engine::GameObject& Player() { return *Objects.Find(PlayerId); }
		Engine::GameObject& Horizontal() { return *Objects.Find(HorizontalId); }
		Engine::GameObject& Vertical() { return *Objects.Find(VerticalId); }
		Engine::GameObject& Enemy() { return *Objects.Find(EnemyId); }

		// One game update, in Game::Update's order.
		void Frame(double dt, bool left = false, bool right = false, bool jump = false)
		{
			Now += dt;
			Controller->BeginFrame();
			Controller->RaiseInputEvents(left, right, jump);
			Events.DispatchDue();

			if (!Replay->IsPlaying())
			{
				Controller->ApplyHorizontalMovement();
				Physics.Update(Player(), static_cast<float>(dt));
				Controller->ResolvePlatformLanding();
				Controller->DetectHazardCollisions();
				Events.DispatchDue();
			}

			// Stands in for UpdateNetworking(): the server keeps moving the platforms (live data
			// that playback must override and, afterwards, restore).
			Horizontal().SetPosition({ 900.0f + static_cast<float>(Now), 600.0f });
			Vertical().SetPosition({ 2450.0f, 500.0f + static_cast<float>(Now) });

			Replay->UpdatePlayback();
			Replay->CaptureFrame();
		}

		void GroundThePlayer()
		{
			Player().SetPosition({ 100.0f, 960.0f });
			Player().SetVelocity({ 0.0f, 100.0f });
			Controller->BeginFrame();
			Controller->ResolvePlatformLanding();
			Events.DispatchDue();
		}

		// Records `frames` updates walking right with a jump at `jumpAt`, optionally dropping
		// into the death zone at `deathAt`, then stops. Returns after the Stop was handled.
		void Record(int frames, int jumpAt, int deathAt = -1)
		{
			GroundThePlayer();
			Replay->RequestStartRecording();
			for (int i = 0; i < frames; ++i)
			{
				if (i == deathAt)
				{
					Player().SetPosition({ 100.0f, 1100.0f });
					Player().SetVelocity({ 0.0f, 500.0f });
				}
				Frame(Dt(i), false, i >= 1, i == jumpAt);
			}
			Replay->RequestStopRecording();
			Frame(Dt(frames));
		}

		void SetDistinctLiveState()
		{
			Player().SetPosition({ 700.0f, 300.0f });
			Player().SetVelocity({ 5.0f, -7.0f });
			Controller->SetGrounded(true);
			Controller->SetCameraOffsetX(ViewWidth);
			Horizontal().SetPosition({ 1111.0f, 222.0f });
			Vertical().SetPosition({ 333.0f, 444.0f });
			Enemy().SetPosition({ 555.0f, 666.0f });
		}

		static bool Same(Engine::Vector2 a, Engine::Vector2 b) { return a.X == b.X && a.Y == b.Y; }

		bool LiveStateIsDistinctState()
		{
			return Same(Player().GetPosition(), { 700.0f, 300.0f }) && Same(Player().GetVelocity(), { 5.0f, -7.0f }) &&
			       Controller->IsGrounded() && Controller->CameraOffsetX() == ViewWidth &&
			       Same(Horizontal().GetPosition(), { 1111.0f, 222.0f }) && Same(Vertical().GetPosition(), { 333.0f, 444.0f }) &&
			       Same(Enemy().GetPosition(), { 555.0f, 666.0f });
		}

		size_t CountDispatched(Engine::EventType type, bool duringPlayback) const
		{
			size_t count = 0;
			for (const Seen& seen : Dispatched)
			{
				count += (seen.Event.Type == type && seen.DuringPlayback == duringPlayback) ? 1 : 0;
			}
			return count;
		}

		double Now = 100.0;
		Engine::EventManager Events;
		Engine::PhysicsSystem Physics;
		Engine::GameObjectRegistry Objects;
		Engine::ObjectId PlayerId = 0;
		Engine::ObjectId HorizontalId = 0;
		Engine::ObjectId VerticalId = 0;
		Engine::ObjectId EnemyId = 0;
		Engine::ObjectId DeathZoneId = 0;
		Engine::ObjectId RemoteId = 0;

		std::vector<Engine::NetworkEventKind> SentToPeers;
		std::vector<Seen> Dispatched;
		std::vector<Engine::ScopedSubscription> Subscriptions;
		std::unique_ptr<LocalPlayerController> Controller;
		std::unique_ptr<PeerEventBridge> Bridge;
		std::unique_ptr<ReplayController> Replay;
	};

	// The frame a correct playback shows at `elapsed`: the latest recorded frame at or before it.
	const ReplayFrame& ExpectedFrame(const std::vector<ReplayFrame>& frames, double elapsed)
	{
		size_t index = 0;
		for (size_t i = 0; i < frames.size(); ++i)
		{
			if (frames[i].RelativeTime <= elapsed)
			{
				index = i;
			}
		}
		return frames[index];
	}

	size_t IndexOfFrameShowing(const std::vector<ReplayFrame>& frames, Engine::Vector2 position)
	{
		for (size_t i = 0; i < frames.size(); ++i)
		{
			if (Scene::Same(frames[i].Sample.PlayerPosition, position))
			{
				return i;
			}
		}
		return frames.size();
	}
}

int main()
{
	constexpr int RecordedFrames = 60;
	constexpr int JumpFrame = 6;

	// 1. F5-equivalent: ReplayStart (an EVENT) starts recording, but only when dispatched.
	{
		Scene scene;
		scene.Replay->RequestStartRecording();
		Check(!scene.Replay->IsRecording(), "Start_RaisingTheEvent_DoesNotYetRecord");
		scene.Events.DispatchDue();
		Check(scene.Replay->IsRecording(), "Start_HandlingTheEvent_StartsRecording");
	}

	// 2 & 3. Frames are captured while recording; F6-equivalent stops and PRESERVES them.
	{
		Scene scene;
		scene.GroundThePlayer();
		scene.Replay->RequestStartRecording();
		for (int i = 0; i < 10; ++i)
		{
			scene.Frame(Dt(i), false, true, false);
		}
		Check(scene.Replay->Frames().size() == 10, "Recording_CapturesOneFramePerUpdate");
		Check(scene.Replay->Frames().front().RelativeTime == 0.0, "Recording_FirstFrameIsAtRelativeTimeZero");

		scene.Replay->RequestStopRecording();
		scene.Frame(Dt(10));
		Check(!scene.Replay->IsRecording() && scene.Replay->Frames().size() == 10, "Stop_EndsRecordingAndPreservesFrames");
		scene.Frame(Dt(11));
		scene.Frame(Dt(12));
		Check(scene.Replay->Frames().size() == 10, "Stop_NoFurtherFramesAreCaptured");
	}

	// 4 & 18. Playback only with a valid recording; empty Play is safe.
	{
		Scene scene;
		scene.Replay->RequestPlayback();
		scene.Frame(Dt(0));
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsPlaybackMode(), "Play_WithNoRecording_IsSafelyIgnored");

		scene.Replay->RequestStartRecording();
		scene.Events.DispatchDue();
		scene.Replay->RequestStopRecording();
		scene.Events.DispatchDue(); // started and stopped in the same instant: empty
		scene.Replay->RequestPlayback();
		scene.Frame(Dt(1));
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsPlaybackMode(), "Play_WithEmptyRecording_IsSafelyIgnored");
		scene.Frame(Dt(2)); // and the game simply carries on live
		Check(scene.Replay->Frames().size() <= 1, "Play_EmptyCase_GameKeepsRunning");
	}

	// 5 & 6. A same-length playback shows the recorded positions, in order, on a NEW time origin.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		std::vector<ReplayFrame> recorded = scene.Replay->Frames();
		Check(recorded.size() == static_cast<size_t>(RecordedFrames), "Replay_SetupRecordedExpectedFrames");

		scene.Now += 400.0; // playback happens at a completely different absolute time
		scene.SetDistinctLiveState();
		scene.Replay->RequestPlayback();

		bool allInOrderAndExact = true;
		bool playedWholeThing = true;
		for (int i = 0; i < RecordedFrames; ++i)
		{
			scene.Frame(i == 0 ? Dt(0) : Dt(i));
			playedWholeThing = playedWholeThing && scene.Replay->IsPlaying();
			allInOrderAndExact = allInOrderAndExact &&
			                     Scene::Same(scene.Player().GetPosition(), recorded[i].Sample.PlayerPosition) &&
			                     Scene::Same(scene.Player().GetVelocity(), recorded[i].Sample.PlayerVelocity) &&
			                     scene.Controller->IsGrounded() == recorded[i].Sample.Grounded &&
			                     scene.Controller->CameraOffsetX() == recorded[i].Sample.CameraOffsetX &&
			                     Scene::Same(scene.Enemy().GetPosition(), recorded[i].Sample.EnemyPosition);
		}
		Check(playedWholeThing, "Replay_StaysInPlaybackForTheWholeRecordedSpan");
		Check(allInOrderAndExact, "Replay_ReproducesRecordedPositionsInOrder_OnANewTimeOrigin");
		Check(scene.Replay->Frames().front().RelativeTime == 0.0, "Replay_UsesRelativeNotAbsoluteTime");
	}

	// 7. Playback frames of any length show only recorded states, in order, matching elapsed time.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		std::vector<ReplayFrame> recorded = scene.Replay->Frames();

		scene.Replay->RequestPlayback();
		scene.Frame(1.0 / 256); // Play handled here: this instant is playback time zero
		double origin = scene.Replay->IsPlaying() ? scene.Now : 0.0;

		bool matchesExpected = true;
		bool onlyRecordedStates = true;
		bool monotone = true;
		size_t lastIndex = 0;
		double widths[] = { 1.0 / 8, 1.0 / 128, 1.0 / 4, 1.0 / 64, 1.0 / 2, 1.0 / 16 };
		for (int i = 0; i < 40 && scene.Replay->IsPlaying(); ++i)
		{
			scene.Frame(widths[i % 6]);
			if (!scene.Replay->IsPlaying())
			{
				break;
			}
			const ReplayFrame& expected = ExpectedFrame(recorded, scene.Now - origin);
			matchesExpected = matchesExpected && Scene::Same(scene.Player().GetPosition(), expected.Sample.PlayerPosition);
			size_t index = IndexOfFrameShowing(recorded, scene.Player().GetPosition());
			onlyRecordedStates = onlyRecordedStates && index < recorded.size();
			monotone = monotone && index >= lastIndex;
			lastIndex = index < recorded.size() ? index : lastIndex;
		}
		Check(matchesExpected, "VariableDt_PlaybackShowsTheFrameForTheElapsedLogicalTime");
		Check(onlyRecordedStates, "VariableDt_PlaybackNeverInventsAState");
		Check(monotone, "VariableDt_PlaybackNeverGoesBackwards");
		Check(!scene.Replay->IsPlaying(), "VariableDt_PlaybackStillCompletes");
	}

	// 8. Zero-length frames during playback: same frame, no advance, no crash.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		scene.Replay->RequestPlayback();
		scene.Frame(1.0 / 32);
		scene.Frame(1.0 / 16);
		Engine::Vector2 before = scene.Player().GetPosition();
		for (int i = 0; i < 25; ++i)
		{
			scene.Frame(0.0);
		}
		Check(scene.Replay->IsPlaying() && Scene::Same(scene.Player().GetPosition(), before),
		      "ZeroDelta_PlaybackHoldsTheSameFrame");
		scene.Frame(1.0 / 16);
		Check(scene.Replay->IsPlaying(), "ZeroDelta_PlaybackResumesAfterwards");
	}

	// 9, 10 & 11. While playing: live input, live physics and live death detection do nothing.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		std::vector<ReplayFrame> recorded = scene.Replay->Frames();

		scene.Replay->RequestPlayback();
		scene.Frame(Dt(0));
		Check(scene.Controller->IsPlaybackMode(), "Playback_PutsThePlayerControllerInPlaybackMode");

		bool trackedRecordingNotLiveInput = true;
		for (int i = 1; i < 20; ++i)
		{
			// Hold LEFT and press JUMP live on every frame: neither may touch the replayed player.
			scene.Frame(Dt(i), true, false, true);
			trackedRecordingNotLiveInput = trackedRecordingNotLiveInput &&
			                               Scene::Same(scene.Player().GetPosition(), recorded[i].Sample.PlayerPosition);
		}
		Check(trackedRecordingNotLiveInput, "Playback_LiveInputAndPhysicsDoNotMoveTheReplayedPlayer");

		// The recording only ever held MoveRight and Jump; a live MoveLeft reaching the event
		// stream during playback would mean live input leaked through.
		bool liveMoveLeftLeaked = false;
		for (const Seen& seen : scene.Dispatched)
		{
			if (seen.DuringPlayback && seen.Event.Type == Engine::EventType::Input &&
			    std::get<Engine::InputEventData>(seen.Event.Payload).Action == Engine::InputAction::MoveLeft)
			{
				liveMoveLeftLeaked = true;
			}
		}
		Check(!liveMoveLeftLeaked, "Playback_LiveInputRaisesNoInputEvents");

		// The controller-level suppression itself, called directly:
		scene.Controller->RaiseInputEvents(true, true, true);
		Check(scene.Events.PendingCount() == 0, "Playback_RaiseInputEvents_RaisesNothing");
		scene.Player().SetPosition({ 100.0f, 1100.0f }); // teleported into the death zone
		scene.Controller->DetectHazardCollisions();
		Check(scene.Events.PendingCount() == 0, "Playback_HazardDetection_RaisesNothing");
		scene.Player().SetPosition({ 100.0f, 990.0f });
		scene.Player().SetVelocity({ 0.0f, 50.0f });
		scene.Controller->ResolvePlatformLanding();
		Check(scene.Events.PendingCount() == 0 && scene.Player().GetPosition().Y == 990.0f,
		      "Playback_LandingResolution_DoesNothing");
		Check(scene.CountDispatched(Engine::EventType::Death, true) == 0,
		      "Playback_LiveDeathDetection_ProducesNoDeath");
	}

	// 12. A replayed Death is dispatched (visible in the event stream) but NEVER announced to peers,
	// and it does not respawn or otherwise act on the live player.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame, 20); // die at frame 20 while recording
		Check(scene.SentToPeers.size() == 1, "Death_LiveDeathDuringRecordingWasAnnouncedOnce");
		size_t recordedDeaths = 0;
		size_t recordedSpawns = 0;
		for (const Engine::RecordedEvent& recorded : scene.Replay->RecordedEvents())
		{
			recordedDeaths += Engine::TypeOf(recorded.Payload) == Engine::EventType::Death ? 1 : 0;
			recordedSpawns += Engine::TypeOf(recorded.Payload) == Engine::EventType::Spawn ? 1 : 0;
		}
		Check(recordedDeaths == 1 && recordedSpawns == 1, "Death_RecordedAsLocalDeathAndSpawn");

		scene.Dispatched.clear();
		scene.Replay->RequestPlayback();
		for (int i = 0; i < RecordedFrames + 5; ++i)
		{
			scene.Frame(Dt(i));
		}
		Check(scene.CountDispatched(Engine::EventType::Death, true) == 1 &&
		          scene.CountDispatched(Engine::EventType::Spawn, true) == 1,
		      "Death_ReplayedDeathAndSpawnAreDispatchedDuringPlayback");
		Check(scene.SentToPeers.size() == 1, "Death_ReplayedDeath_IsNotRebroadcastToPeers");
		Check(scene.CountDispatched(Engine::EventType::Collision, true) == 0,
		      "Death_CollisionIsNotRedispatchedDuringPlayback");
	}

	// 13. Replay never records itself.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame, 20);
		size_t eventsBefore = scene.Replay->RecordedEvents().size();
		size_t framesBefore = scene.Replay->Frames().size();
		scene.Replay->RequestPlayback();
		for (int i = 0; i < RecordedFrames + 5; ++i)
		{
			scene.Frame(Dt(i));
		}
		Check(!scene.Replay->IsPlaying() && scene.Replay->RecordedEvents().size() == eventsBefore &&
		          scene.Replay->Frames().size() == framesBefore,
		      "NoRecursion_PlaybackAddsNothingToTheRecording");

		// Start/Stop/Play pressed during playback are ignored; playback is not disturbed.
		scene.Replay->RequestPlayback();
		scene.Frame(Dt(0));
		scene.Replay->RequestStartRecording();
		scene.Replay->RequestStopRecording();
		scene.Replay->RequestPlayback();
		scene.Frame(Dt(1));
		Check(scene.Replay->IsPlaying() && !scene.Replay->IsRecording() && scene.Replay->Frames().size() == framesBefore,
		      "NoRecursion_ControlKeysDuringPlayback_AreIgnored");
	}

	// 14, 15 & 16. Completion restores the whole live state: player, velocity, grounded, camera,
	// platforms, enemy.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		scene.SetDistinctLiveState();
		scene.Replay->RequestPlayback();
		scene.Frame(Dt(0));
		Check(scene.Replay->IsPlaying() && !scene.LiveStateIsDistinctState(), "Restore_PlaybackReplacesTheLiveStateWhilePlaying");
		Check(Scene::Same(scene.Replay->SavedLiveSample().PlayerPosition, { 700.0f, 300.0f }),
		      "Restore_LiveStateWasSavedWhenPlaybackBegan");

		int guard = 0;
		while (scene.Replay->IsPlaying() && guard++ < 500)
		{
			scene.Frame(1.0 / 32);
		}
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsPlaybackMode(), "Restore_PlaybackEndsAndModeIsCleared");
		Check(Scene::Same(scene.Player().GetPosition(), { 700.0f, 300.0f }) && Scene::Same(scene.Player().GetVelocity(), { 5.0f, -7.0f }),
		      "Restore_PlayerPositionAndVelocityReturnToLiveState");
		Check(scene.Controller->CameraOffsetX() == ViewWidth, "Restore_CameraReturnsToLiveState");
		Check(scene.Controller->IsGrounded(), "Restore_GroundedReturnsToLiveState");
		Check(Scene::Same(scene.Horizontal().GetPosition(), { 1111.0f, 222.0f }) &&
		          Scene::Same(scene.Vertical().GetPosition(), { 333.0f, 444.0f }) &&
		          Scene::Same(scene.Enemy().GetPosition(), { 555.0f, 666.0f }),
		      "Restore_PlatformsAndEnemyReturnToLiveState");
	}

	// 17. A replay can be played again after completing, with the same result.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		std::vector<ReplayFrame> recorded = scene.Replay->Frames();
		bool bothRunsExact = true;
		for (int run = 0; run < 2; ++run)
		{
			scene.Now += 50.0;
			scene.SetDistinctLiveState();
			scene.Replay->RequestPlayback();
			for (int i = 0; i < RecordedFrames; ++i)
			{
				scene.Frame(Dt(i));
				bothRunsExact = bothRunsExact && scene.Replay->IsPlaying() &&
				                Scene::Same(scene.Player().GetPosition(), recorded[i].Sample.PlayerPosition);
			}
			int guard = 0;
			while (scene.Replay->IsPlaying() && guard++ < 500)
			{
				scene.Frame(1.0 / 32);
			}
			bothRunsExact = bothRunsExact && scene.LiveStateIsDistinctState();
		}
		Check(bothRunsExact, "Again_SecondPlaybackReproducesTheSameFramesAndRestoresAgain");
	}

	// 19. Local Death -> Spawn works normally outside replay: before, and again after, a playback.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		scene.Dispatched.clear();
		scene.SentToPeers.clear();

		scene.Player().SetPosition({ 100.0f, 1100.0f });
		scene.Frame(1.0 / 64);
		Check(scene.CountDispatched(Engine::EventType::Death, false) == 1 && Scene::Same(scene.Player().GetPosition(), Spawn) &&
		          scene.SentToPeers.size() == 1,
		      "Live_DeathThenSpawnWorksOutsideReplay_AndIsAnnounced");

		scene.Replay->RequestPlayback();
		int guard = 0;
		scene.Frame(1.0 / 64);
		while (scene.Replay->IsPlaying() && guard++ < 500)
		{
			scene.Frame(1.0 / 32);
		}
		scene.Dispatched.clear();
		scene.SentToPeers.clear();
		scene.Player().SetPosition({ 100.0f, 1100.0f });
		scene.Player().SetVelocity({ 0.0f, 500.0f });
		scene.Frame(1.0 / 64);
		Check(scene.CountDispatched(Engine::EventType::Death, false) == 1 && Scene::Same(scene.Player().GetPosition(), Spawn),
		      "Live_DeathThenSpawnStillWorksAfterAPlayback");
		Check(scene.SentToPeers.size() == 1, "Live_DeathIsAnnouncedAgainAfterAPlayback");
	}

	// 20. The networked-Death marker behaves the same outside replay — and remote players stay
	// live during a local playback.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);

		scene.Bridge->HandleNetworkEvent(Engine::NetworkEvent{ Engine::NetworkEventKind::Death, RemoteNetworkId });
		scene.Frame(1.0 / 64);
		Check(scene.Bridge->IsMarkedDead(scene.RemoteId), "Marker_RemoteDeathMarksPlayerOutsideReplay");
		scene.Now += PeerEventBridge::MarkDuration + 0.5;
		Check(!scene.Bridge->IsMarkedDead(scene.RemoteId), "Marker_StillExpiresOnLogicalTime");

		scene.Replay->RequestPlayback();
		scene.Frame(1.0 / 64);
		Check(scene.Replay->IsPlaying(), "Marker_SetupPlaybackActive");
		scene.Bridge->HandleNetworkEvent(Engine::NetworkEvent{ Engine::NetworkEventKind::Death, RemoteNetworkId });
		scene.Frame(1.0 / 64);
		Check(scene.Bridge->IsMarkedDead(scene.RemoteId), "Marker_RemoteDeathStillMarksPlayerDuringPlayback");
		Check(scene.Replay->IsPlaying() && scene.SentToPeers.empty(), "Marker_RemoteDeathDuringPlayback_IsNotRebroadcast");
	}

	// A local-only recording filter: a remote player's death is never part of MY replay.
	{
		Scene scene;
		scene.GroundThePlayer();
		scene.Replay->RequestStartRecording();
		scene.Frame(Dt(0));
		scene.Bridge->HandleNetworkEvent(Engine::NetworkEvent{ Engine::NetworkEventKind::Death, RemoteNetworkId });
		scene.Frame(Dt(1));
		scene.Frame(Dt(2));
		scene.Replay->RequestStopRecording();
		scene.Frame(Dt(3));
		bool noRemoteDeath = true;
		for (const Engine::RecordedEvent& recorded : scene.Replay->RecordedEvents())
		{
			if (Engine::TypeOf(recorded.Payload) == Engine::EventType::Death)
			{
				noRemoteDeath = false;
			}
		}
		Check(scene.CountDispatched(Engine::EventType::Death, false) == 1 && noRemoteDeath,
		      "Filter_RemotePlayersDeathIsNotRecordedInTheLocalReplay");
	}

	// Pausing the Timeline (logical time frozen) holds a playback in place.
	{
		Scene scene;
		scene.Record(RecordedFrames, JumpFrame);
		scene.Replay->RequestPlayback();
		scene.Frame(1.0 / 32);
		scene.Frame(1.0 / 16);
		Engine::Vector2 held = scene.Player().GetPosition();
		for (int i = 0; i < 50; ++i)
		{
			scene.Frame(0.0); // paused: logical time does not move
		}
		Check(scene.Replay->IsPlaying() && Scene::Same(scene.Player().GetPosition(), held), "Pause_FrozenLogicalTimeHoldsPlayback");
	}

	if (g_Failures == 0)
	{
		std::printf("All tests passed.\n");
		return 0;
	}

	std::printf("%d test(s) failed.\n", g_Failures);
	return 1;
}
