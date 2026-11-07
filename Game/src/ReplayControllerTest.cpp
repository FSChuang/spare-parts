// Tests for ReplayController (Astro rebuild Checkpoint D): recording local ship + bullet samples,
// playback, the live-state restore, and everything playback must keep OUT of the live game.
// Uses the REAL controller, the REAL Engine::ReplayRecorder, ShipController, BulletField,
// PeerEventBridge and GameObjects, an Engine::EventManager on a fake clock, and a Scene::Frame()
// that runs the same steps in the same order as Game::Update — so what is proven is the actual
// replay flow, not a re-implementation of it. (The one thing a headless test cannot reach is
// Game::Update's own `if (!playing)` branch and its F-key polling; Scene::Frame mirrors them and
// the manual checklist covers the rest.) No SDL, no sockets.
//
// The ship is configured with round, test-local numbers (speed 100, rotation 2 rad/s, cooldown
// 0.5 s, respawn 2 s, invulnerability 1 s) and bullets {speed 200, lifetime 1.4 s, radius 5}, so
// every expectation is written out by hand.

#include "ArenaRules.h"
#include "BulletField.h"
#include "PeerEventBridge.h"
#include "ReplayController.h"
#include "ShipController.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

	// A precondition the rest of a test indexes into: on failure report it and stop, instead of
	// reading past the end of an empty vector.
	void Require(bool condition, const char* name)
	{
		Check(condition, name);
		if (!condition)
		{
			std::printf("Cannot continue: a precondition failed.\n");
			std::exit(1);
		}
	}

	bool Near(float a, float b, float tolerance = 1e-3f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	constexpr Engine::PlayerId LocalNetworkId = 1;
	constexpr Engine::PlayerId RemoteNetworkId = 7;
	constexpr Engine::Vector2 SpawnCentre{ 480.0f, 540.0f }; // spawn heading is exactly 0 (right)
	constexpr Engine::Vector2 OtherSpawn{ 1440.0f, 540.0f };
	constexpr Engine::Vector2 ShipSize{ 48.0f, 48.0f };
	constexpr ShipConfig TestShip{ 100.0f, 2.0f, 24.0f, 36.0f, 0.5, 2.0, 1.0 };

	struct Scene
	{
		Scene()
			: Events([this] { return Now; }),
			  Physics(0.0f),
			  Bullets(Objects, Physics, BulletConfig{ 200.0f, 1.4f, 5.0f })
		{
			Engine::GameObject& ship = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
			SetCenter(ship, SpawnCentre);
			ship.SetVelocity({ 0.0f, 0.0f });
			ship.SetNetworkId(LocalNetworkId);
			ShipId = ship.GetId();

			Engine::GameObject& remote = Objects.Create(Engine::ObjectType::Player, { 0.0f, 0.0f }, ShipSize);
			SetCenter(remote, { 1500.0f, 200.0f });
			remote.SetNetworkId(RemoteNetworkId);
			RemoteId = remote.GetId();

			Controller = std::make_unique<ShipController>(Events, Objects, Bullets, ShipId, TestShip,
			                                              [this] { return NextSpawn; }, [this] { return Now; });
			Bridge = std::make_unique<PeerEventBridge>(Events, Objects, Bullets, ShipId, [this] { return Now; },
			                                          [this](const Engine::NetworkEvent& event) { Sent.push_back(event); });
			Controller->SetShotListener([this](Engine::Vector2 origin, float heading) { Bridge->AnnounceShot(origin, heading); });
			Replay = std::make_unique<ReplayController>(Events, Objects, Bullets, *Controller, *Bridge, ShipId,
			                                            [this] { return Now; });

			for (Engine::EventType type : { Engine::EventType::Input, Engine::EventType::Collision,
			                                 Engine::EventType::Death, Engine::EventType::Spawn })
			{
				Subscriptions.emplace_back(Events, type, [this](const Engine::Event& event) { Seen.push_back(event); });
			}
		}

		Engine::GameObject& Ship() { return *Objects.Find(ShipId); }
		Engine::Vector2 Centre() { return CenterOf(Ship()); }

		// One game update, step for step what Game::Update does (see its frame-order comment),
		// with the remote notifications "received this frame" handled where Game handles them.
		void Frame(float dt, bool left = false, bool right = false, bool fire = false)
		{
			Now += dt;
			Controller->BeginFrame();
			Controller->RaiseInputEvents(left, right, fire);
			Events.DispatchDue();

			if (!Replay->IsPlaying())
			{
				Controller->ApplySteering(dt);
				Physics.Update(Ship(), dt);
				Controller->WrapAroundArena();
				Bullets.Update(dt);
				Controller->DetectBulletHits();
				Events.DispatchDue();
			}

			for (const Engine::NetworkEvent& event : Incoming)
			{
				Bridge->HandleNetworkEvent(event);
			}
			Incoming.clear();

			Published = ShipObjectState();
			if (Replay->IsPlaying())
			{
				Published = Replay->SavedLiveShip(); // what Game publishes instead of the replayed ship
			}

			Replay->UpdatePlayback();
			Replay->CaptureFrame();
		}

		// A frame in which nothing but time passes, repeated.
		void Run(float seconds, float step)
		{
			for (float t = 0.0f; t < seconds - 1e-4f; t += step)
			{
				Frame(step);
			}
		}

		// Plays on, in steps of `step`, until playback has just completed; stops right there, so
		// the state seen afterwards is exactly the state at completion (no extra live frame).
		void RunUntilDone(float step)
		{
			for (int i = 0; i < 1000 && Replay->IsPlaying(); ++i)
			{
				Frame(step);
			}
		}

		ShipSnapshot ShipObjectState() { return Controller->Capture(); }

		// Alive ship at SpawnCentre (heading 0), optionally with the spawn protection already run out.
		void StartShip(bool vulnerable = true)
		{
			Controller->RequestSpawn(SpawnCentre);
			Events.DispatchDue();
			if (vulnerable)
			{
				Now += 1.0;
			}
			Seen.clear();
			Sent.clear();
		}

		int Count(Engine::EventType type) const
		{
			int n = 0;
			for (const Engine::Event& event : Seen)
			{
				n += event.Type == type ? 1 : 0;
			}
			return n;
		}

		// Records [Start .. Stop]: the controls go through the EventManager, and the frames in
		// `body` are captured. Returns once recording has stopped.
		template <typename Body>
		void RecordFor(Body body)
		{
			Replay->RequestStartRecording();
			Frame(0.0f); // dispatches ReplayStart; this frame is the first sample (relative time 0)
			body();
			Replay->RequestStopRecording();
			Frame(0.0f); // dispatches ReplayStop (no sample is taken once stopped)
		}

		static Engine::NetworkEvent RemoteDeath() { return { Engine::NetworkEventKind::Death, RemoteNetworkId }; }
		static Engine::NetworkEvent RemoteSpawn() { return { Engine::NetworkEventKind::Spawn, RemoteNetworkId }; }
		static Engine::NetworkEvent RemoteShoot(float x, float y)
		{
			Engine::NetworkEvent shot{ Engine::NetworkEventKind::Shoot, RemoteNetworkId };
			shot.OriginX = x;
			shot.OriginY = y;
			shot.HeadingRadians = 0.0f;
			return shot;
		}

		double Now = 0.0;
		Engine::Vector2 NextSpawn = OtherSpawn;
		Engine::EventManager Events;
		Engine::GameObjectRegistry Objects;
		Engine::PhysicsSystem Physics;
		BulletField Bullets;
		Engine::ObjectId ShipId = 0;
		Engine::ObjectId RemoteId = 0;
		std::vector<Engine::NetworkEvent> Sent;
		std::vector<Engine::NetworkEvent> Incoming;
		ShipSnapshot Published{};
		std::vector<Engine::Event> Seen;
		std::vector<Engine::ScopedSubscription> Subscriptions;
		std::unique_ptr<ShipController> Controller;
		std::unique_ptr<PeerEventBridge> Bridge;
		std::unique_ptr<ReplayController> Replay;
	};

	// A hostile bullet nobody owns, parked on the ship; the real hit path finds it.
	void ShootShip(Scene& scene)
	{
		scene.Bullets.Fire(scene.Centre(), 0.0f, Engine::NoObjectId);
	}

	// A simple recording used by many tests: the ship flies straight right for 1.4 s of logical
	// time, sampled at rel 0.0 (x=480), 0.5 (x=530) and 1.0 (x=580), then stopped at 1.4 so the
	// last sample is held for 0.4 s. Everything is deterministic from StartShip().
	void RecordStraightFlight(Scene& scene)
	{
		scene.RecordFor(
		    [&]
		    {
			    scene.Frame(0.5f);
			    scene.Frame(0.5f);
			    scene.Frame(0.4f); // not sampled as a new position: the stop below lands at rel 1.4
		    });
	}
}

int main()
{
	// ======== Recording ========
	{
		Scene scene;
		scene.StartShip();
		scene.Replay->RequestStartRecording();
		Check(!scene.Replay->IsRecording() && scene.Events.PendingCount() == 1, "Record_F5_OnlyRaisesAControlEvent");
		scene.Frame(0.0f);
		Check(scene.Replay->IsRecording(), "Record_StartTakesEffectWhenTheEventIsDispatched");
		Check(scene.Replay->Frames().size() == 1 && scene.Replay->Frames()[0].RelativeTime == 0.0,
		      "Record_TheFrameThatStartedItIsTheFirstSample");
	}
	{
		Scene scene;
		scene.StartShip();
		scene.RecordFor([&] { scene.Frame(0.5f); scene.Frame(0.5f); });
		Require(scene.Replay->Frames().size() == 3 && !scene.Replay->IsRecording(), "Record_ThreeSamplesThenStopped");
		const auto& frames = scene.Replay->Frames();
		Check(frames[0].RelativeTime == 0.0 && frames[1].RelativeTime == 0.5 && frames[2].RelativeTime == 1.0,
		      "Record_SampleTimesAreRelativeLogicalTime");
		Check(Near(frames[0].Sample.ShipCentre.X, 480.0f) && Near(frames[1].Sample.ShipCentre.X, 530.0f) &&
		          Near(frames[2].Sample.ShipCentre.X, 580.0f) && Near(frames[2].Sample.ShipCentre.Y, 540.0f),
		      "Record_SamplesShipPosition");
		Check(Near(frames[1].Sample.ShipVelocity.X, 100.0f) && Near(frames[1].Sample.ShipVelocity.Y, 0.0f),
		      "Record_SamplesShipVelocity");
		Check(frames[0].Sample.Alive && frames[2].Sample.Alive, "Record_SamplesAliveState");
	}
	{
		Scene scene; // heading
		scene.StartShip();
		scene.RecordFor([&] { scene.Frame(0.25f, false, true, false); });
		Require(scene.Replay->Frames().size() == 2, "Record_Heading_TwoSamples");
		Check(Near(scene.Replay->Frames()[0].Sample.HeadingRadians, 0.0f) && Near(scene.Replay->Frames()[1].Sample.HeadingRadians, 0.5f),
		      "Record_SamplesHeading");
	}
	{
		Scene scene; // the invulnerability presentation flag
		scene.StartShip(false); // just spawned: protected for 1.0 s
		scene.RecordFor([&] { scene.Frame(0.5f); scene.Frame(0.5f); });
		Require(scene.Replay->Frames().size() == 3, "Record_Invulnerability_ThreeSamples");
		const auto& frames = scene.Replay->Frames();
		Check(frames[0].Sample.Invulnerable && frames[1].Sample.Invulnerable, "Record_SamplesInvulnerableWhileProtected");
		Check(!frames[2].Sample.Invulnerable, "Record_SamplesNotInvulnerableOnceItRunsOut");
	}
	{
		Scene scene; // local bullets only
		scene.StartShip();
		scene.RecordFor(
		    [&]
		    {
			    scene.Frame(0.0f, false, false, true); // fires: bullet at the muzzle, x=516
			    scene.Incoming.push_back(Scene::RemoteShoot(900.0f, 100.0f));
			    scene.Frame(0.5f); // the bullet moves 100 px; a remote bullet is rebuilt too
		    });
		Require(scene.Replay->Frames().size() == 3, "Record_Bullets_ThreeSamples");
		const auto& frames = scene.Replay->Frames();
		Check(frames[0].Sample.Bullets.empty(), "Record_NoBulletsBeforeFiring");
		Require(frames[1].Sample.Bullets.size() == 1, "Record_LocalBulletIsSampled");
		Check(Near(frames[1].Sample.Bullets[0].Centre.X, 516.0f) && Near(frames[1].Sample.Bullets[0].Centre.Y, 540.0f),
		      "Record_BulletPositionIsTheMuzzlePoint");
		Require(frames[2].Sample.Bullets.size() == 1, "Record_RemoteBulletIsNotSampled_OnlyTheLocalOneRemains");
		Check(Near(frames[2].Sample.Bullets[0].Centre.X, 616.0f), "Record_BulletPositionFollowsItsFlight");
		Check(scene.Bullets.All().size() == 2, "Record_TheRemoteBulletReallyExistedInTheLiveField");
	}
	{
		Scene scene; // F6 stops; an idle F6 is harmless; a second recording replaces the first
		scene.StartShip();
		scene.Replay->RequestStopRecording();
		scene.Frame(0.1f);
		Check(!scene.Replay->IsRecording() && scene.Replay->Frames().empty(), "Record_IdleF6_Harmless");
		scene.RecordFor([&] { scene.Frame(0.5f); });
		std::size_t firstCount = scene.Replay->Frames().size();
		scene.Frame(0.5f);
		Check(scene.Replay->Frames().size() == firstCount, "Record_StoppedRecordingTakesNoMoreSamples");
		scene.Replay->RequestStartRecording();
		scene.Frame(0.0f);
		Check(scene.Replay->IsRecording() && scene.Replay->Frames().size() == 1, "Record_SecondRecordingClearsTheFirst");
		scene.Frame(0.5f);
		scene.Replay->RequestStartRecording(); // F5 while recording restarts
		scene.Frame(0.0f);
		Check(scene.Replay->Frames().size() == 1 && scene.Replay->Frames()[0].RelativeTime == 0.0, "Record_RepeatedF5_Restarts");
	}

	// ======== Event filter ========
	{
		Scene scene;
		scene.StartShip();
		scene.Replay->RequestStartRecording();
		scene.Frame(0.0f);
		scene.Frame(0.1f, false, true, false);                        // local Input
		ShootShip(scene);                                             // hits the local ship: Collision -> Death
		scene.Frame(0.1f);
		scene.Incoming.push_back(Scene::RemoteDeath());               // remote Death
		scene.Incoming.push_back(Scene::RemoteSpawn());               // remote Spawn
		scene.Frame(0.1f);
		scene.Now += 2.0;                                             // local respawn comes due
		scene.Frame(0.0f);
		scene.Replay->RequestStopRecording();
		scene.Frame(0.0f);

		int input = 0, collision = 0, localDeath = 0, remoteDeath = 0, localSpawn = 0, remoteSpawn = 0, control = 0;
		for (const Engine::RecordedEvent& recorded : scene.Replay->RecordedEvents())
		{
			switch (static_cast<Engine::EventType>(recorded.Payload.index()))
			{
			case Engine::EventType::Input:
				++input;
				break;
			case Engine::EventType::Collision:
				++collision;
				break;
			case Engine::EventType::Death:
				(std::get<Engine::DeathEventData>(recorded.Payload).Subject == scene.ShipId ? localDeath : remoteDeath)++;
				break;
			case Engine::EventType::Spawn:
				(std::get<Engine::SpawnEventData>(recorded.Payload).Subject == scene.ShipId ? localSpawn : remoteSpawn)++;
				break;
			default:
				++control;
			}
		}
		Check(input >= 1, "Filter_LocalInputRecorded");
		Check(collision == 1, "Filter_LocalCollisionRetainedInTheLog");
		Check(localDeath == 1 && remoteDeath == 0, "Filter_LocalDeathRecorded_RemoteDeathExcluded");
		Check(localSpawn == 1 && remoteSpawn == 0, "Filter_LocalSpawnRecorded_RemoteSpawnExcluded");
		Check(control == 0, "Filter_ControlEventsNeverRecorded");
		Check(scene.Count(Engine::EventType::Death) == 2, "Filter_TheRemoteDeathDidHappenLive_ItWasJustNotRecorded");
	}

	// ======== Playback: frame selection and variable time ========
	{
		Scene scene;
		scene.StartShip();
		RecordStraightFlight(scene);
		Require(scene.Replay->Frames().size() == 4, "Playback_Setup_FourSamples"); // rel 0, 0.5, 1.0, 1.4
		scene.Frame(1.0f); // live play moves on (the real ship ends up somewhere else)

		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Require(scene.Replay->IsPlaying(), "Playback_F7_StartsPlayback");
		Check(Near(scene.Centre().X, 480.0f), "Frame_AtElapsedZero_FirstSampleShown");

		scene.Frame(0.25f); // elapsed 0.25: between samples, the earlier one holds
		Check(Near(scene.Centre().X, 480.0f), "Frame_BetweenSamples_EarlierOneHolds_NoInterpolation");
		scene.Frame(0.25f); // elapsed 0.5 (exact in binary): exactly the second sample's time
		Check(Near(scene.Centre().X, 530.0f), "Frame_ExactTimestamp_ThatSampleIsShown");
		scene.Frame(0.0f); // zero dt: nothing changes
		Check(Near(scene.Centre().X, 530.0f) && scene.Replay->IsPlaying(), "Frame_ZeroDt_HoldsTheSameSample");
		scene.Frame(0.45f); // elapsed 0.95
		Check(Near(scene.Centre().X, 530.0f), "Frame_JustBeforeTheNextSample_StillHeld");
		scene.Frame(0.1f); // elapsed 1.05: 1.0 sample due
		Check(Near(scene.Centre().X, 580.0f), "Frame_IrregularDt_ShowsOnlyRecordedStates");
		scene.Frame(0.2f); // elapsed 1.25: final sample (rel 1.4) not due, 1.0 sample holds
		Check(Near(scene.Centre().X, 580.0f) && scene.Replay->IsPlaying(), "Frame_PastTheLastSample_ItIsHeldUntilTheEnd");
		scene.Frame(0.1f); // elapsed 1.35 — recording ended at 1.4, so still playing
		Check(scene.Replay->IsPlaying(), "Frame_BeforeTheRecordedDurationElapses_StillPlaying");
		scene.Frame(0.1f); // elapsed 1.45 >= 1.4: complete
		Check(!scene.Replay->IsPlaying(), "Completion_WhenTheRecordedDurationHasElapsed");
	}
	{
		Scene scene; // a long single step skips straight to the frame in force
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Frame(1.2f);
		Check(Near(scene.Centre().X, 580.0f) && scene.Replay->IsPlaying(), "Frame_OneLongStep_JumpsToTheLastRecordedSample");
	}
	{
		Scene scene; // pause: a frozen clock freezes playback
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Frame(0.6f);
		for (int i = 0; i < 500; ++i)
		{
			scene.Frame(0.0f);
		}
		Check(scene.Replay->IsPlaying() && Near(scene.Centre().X, 530.0f), "Pause_FrozenClock_PlaybackHoldsItsFrame");
		scene.Frame(0.9f); // time resumes: 1.5 elapsed
		Check(!scene.Replay->IsPlaying(), "Pause_ThenResume_PlaybackCompletes");
	}
	{
		Scene scene; // time scale: the same recording plays in half the real time at 2x
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		for (int i = 0; i < 4; ++i)
		{
			scene.Frame(0.2f); // scaled 2x: each frame advances logical time by 0.2
		}
		Check(Near(scene.Centre().X, 530.0f) && scene.Replay->IsPlaying(), "TimeScale_LogicalTimeDrivesPlayback");
	}
	{
		Scene scene; // control commands during playback are ignored
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Frame(0.5f);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Frame(0.6f);
		ShipSnapshot savedBefore = scene.Replay->SavedLiveShip();
		scene.Replay->RequestPlayback();      // F7 again
		scene.Replay->RequestStartRecording(); // F5
		scene.Replay->RequestStopRecording();  // F6
		scene.Frame(0.0f);
		Check(scene.Replay->IsPlaying() && !scene.Replay->IsRecording(), "Controls_DuringPlayback_F5F6F7Ignored");
		Check(Near(scene.Centre().X, 530.0f), "Controls_RepeatedF7_DoesNotRestartPlayback");
		Check(scene.Replay->SavedLiveShip().Position.X == savedBefore.Position.X, "Controls_RepeatedF7_KeepsTheOriginalSavedState");
	}
	{
		Scene scene; // F7 with nothing recorded does nothing
		scene.StartShip();
		scene.Replay->RequestPlayback();
		scene.Frame(0.1f);
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsPlaybackMode(), "Playback_NothingRecorded_DoesNotStart");
		scene.Replay->RequestStartRecording();
		scene.Replay->RequestPlayback(); // F7 mid-recording stops it and plays what there is
		scene.Frame(0.0f);
		scene.Frame(0.2f);
		Check(!scene.Replay->IsRecording(), "Playback_F7MidRecording_StopsTheRecordingFirst");
	}

	// ======== Live state: saved and restored ========
	{
		Scene scene;
		scene.StartShip();
		RecordStraightFlight(scene); // recorded heading is 0, ship moving right
		scene.Frame(0.0f, false, true, false);
		scene.Frame(0.25f, false, true, false); // live: turned to heading 0.5, then it flies on
		scene.Frame(0.5f);
		ShipSnapshot before = scene.ShipObjectState();

		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(Near(scene.Controller->Heading(), 0.0f) && !Near(before.HeadingRadians, 0.0f), "Restore_PlaybackShowsTheRecordedHeading");
		Check(Near(scene.Replay->SavedLiveShip().HeadingRadians, before.HeadingRadians) &&
		          Near(scene.Replay->SavedLiveShip().Position.X, before.Position.X) &&
		          Near(scene.Replay->SavedLiveShip().Position.Y, before.Position.Y),
		      "Restore_TheSavedLiveStateIsWhatTheShipWasBeforePlayback");
		Check(!Near(scene.ShipObjectState().Position.X, before.Position.X), "Restore_DuringPlaybackTheShipObjectShowsTheReplay");

		scene.RunUntilDone(0.1f);
		Check(!scene.Replay->IsPlaying(), "Restore_Setup_PlaybackFinished");
		ShipSnapshot after = scene.ShipObjectState();
		Check(Near(after.Position.X, before.Position.X) && Near(after.Position.Y, before.Position.Y), "Restore_PositionRestored");
		Check(Near(after.Velocity.X, before.Velocity.X) && Near(after.Velocity.Y, before.Velocity.Y), "Restore_VelocityRestored");
		Check(Near(after.HeadingRadians, before.HeadingRadians), "Restore_HeadingRestored");
		Check(after.Alive == before.Alive && after.Alive, "Restore_AliveRestored");
	}
	{
		Scene scene; // a recording that contains a death still plays back; the live ship is untouched by it
		scene.StartShip();
		scene.Replay->RequestStartRecording();
		scene.Frame(0.0f);
		scene.Frame(0.3f);
		ShootShip(scene);
		scene.Frame(0.0f); // hit: dead
		scene.Frame(1.0f);
		scene.Frame(1.0f); // 2.0 s after the hit: the respawn comes due
		scene.Frame(0.5f);
		scene.Replay->RequestStopRecording();
		scene.Frame(0.0f);
		bool recordedDead = false;
		for (const ReplayFrame& frame : scene.Replay->Frames())
		{
			recordedDead = recordedDead || !frame.Sample.Alive;
		}
		Check(recordedDead && scene.Controller->IsAlive(), "Restore_Setup_RecordingContainsADeathAndTheShipIsAliveAgain");

		scene.Frame(0.5f);
		ShipSnapshot before = scene.ShipObjectState();
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		bool shownDead = false;
		for (int i = 0; i < 80 && scene.Replay->IsPlaying(); ++i)
		{
			scene.Frame(0.05f);
			shownDead = shownDead || !scene.Controller->IsAlive();
		}
		Check(shownDead, "Playback_ShowsTheRecordedDeathAsADeadShip");
		ShipSnapshot after = scene.ShipObjectState();
		Check(!scene.Replay->IsPlaying() && after.Alive && Near(after.Position.X, before.Position.X), "Playback_ThenTheLiveAliveShipIsBack");
	}

	// ======== Timers: remaining durations, rebased ========
	{
		Scene scene; // cooldown with 0.3 s left
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Frame(0.0f, false, false, true); // fires: cooldown 0.5 s
		scene.Frame(0.2f);                     // 0.3 s left
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(Near(static_cast<float>(scene.Replay->SavedLiveShip().CooldownRemaining), 0.3f), "Timers_CooldownRemainingCaptured");
		scene.RunUntilDone(0.1f);
		Require(!scene.Replay->IsPlaying(), "Timers_Setup_PlaybackFinished");
		std::size_t bulletsBefore = scene.Bullets.All().size();
		scene.Frame(0.29f, false, false, true);
		Check(scene.Bullets.All().size() == bulletsBefore, "Timers_RemainingCooldown_StillBlocksJustAfterPlayback");
		scene.Frame(0.02f, false, false, true); // 0.31 s after the end: more than the 0.3 s that remained
		Check(scene.Bullets.All().size() == bulletsBefore + 1, "Timers_CooldownEndsAfterTheRemainingTime_NotSoonerNotLater");
	}
	{
		Scene scene; // a finished cooldown stays finished
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Frame(0.0f, false, false, true);
		scene.Frame(0.6f); // 0.5 s cooldown has run out
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(scene.Replay->SavedLiveShip().CooldownRemaining == 0.0, "Timers_FinishedCooldownCapturedAsZeroRemaining");
		scene.RunUntilDone(0.1f);
		std::size_t bulletsBefore = scene.Bullets.All().size();
		scene.Frame(0.0f, false, false, true);
		Check(scene.Bullets.All().size() == bulletsBefore + 1, "Timers_FinishedCooldown_ShootsImmediatelyAfterPlayback");
	}
	{
		Scene scene; // invulnerability with 0.7 s left
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Controller->RequestSpawn(SpawnCentre); // a fresh spawn: protected for 1.0 s
		scene.Frame(0.0f);
		scene.Frame(0.3f);                           // 0.7 s left
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(Near(static_cast<float>(scene.Replay->SavedLiveShip().InvulnerabilityRemaining), 0.7f), "Timers_InvulnerabilityRemainingCaptured");
		scene.RunUntilDone(0.1f);
		Require(!scene.Replay->IsPlaying(), "Timers_Invuln_PlaybackFinished");
		Check(scene.Controller->IsInvulnerable(), "Timers_InvulnerabilitySurvivesPlayback");
		scene.Frame(0.69f);
		Check(scene.Controller->IsInvulnerable(), "Timers_InvulnerabilityLastsTheRemainingTime");
		scene.Frame(0.02f);
		Check(!scene.Controller->IsInvulnerable(), "Timers_InvulnerabilityEndsAfterExactlyTheRemainingTime");
	}
	{
		Scene scene; // expired invulnerability stays expired
		scene.StartShip();
		RecordStraightFlight(scene);
		Check(!scene.Controller->IsInvulnerable(), "Timers_Setup_NotProtected");
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.RunUntilDone(0.1f);
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsInvulnerable(), "Timers_ExpiredInvulnerabilityStaysExpired");
	}

	// ======== Live bullets: frozen, then resumed ========
	{
		Scene scene;
		scene.StartShip();
		scene.RecordFor([&] { scene.Frame(1.0f); scene.Frame(1.0f); }); // a 2 s recording, far longer than a bullet lives
		scene.Frame(0.0f, false, false, true);  // a live bullet at x=516, flying right at 200 px/s
		scene.Frame(0.2f);                      // x=556, age 0.2
		Require(scene.Bullets.All().size() == 1, "Bullets_Setup_OneLiveBullet");
		Engine::ObjectId bulletId = scene.Bullets.All()[0].Id;
		float bulletX = CenterOf(*scene.Objects.Find(bulletId)).X;
		float bulletAge = scene.Bullets.All()[0].Age;
		std::size_t objectCount = scene.Objects.Count();

		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Run(1.9f, 0.1f); // 1.9 s of playback: the bullet would have died at 1.4 s of age
		Require(scene.Replay->IsPlaying(), "Bullets_Setup_StillPlaying");
		Check(scene.Bullets.All().size() == 1 && scene.Bullets.Contains(bulletId), "Bullets_LiveBulletNotExpiredDuringPlayback");
		Check(Near(CenterOf(*scene.Objects.Find(bulletId)).X, bulletX), "Bullets_LiveBulletPositionFrozen");
		Check(Near(scene.Bullets.All()[0].Age, bulletAge), "Bullets_LiveBulletAgeFrozen");
		Check(scene.Objects.Count() == objectCount, "Bullets_PlaybackCreatesNoGameObjectsAndNoLiveBullets");
		Check(scene.Replay->CurrentSample().Bullets.empty(), "Bullets_ReplayBulletsComeFromTheSample_ThisRecordingHadNone");

		scene.RunUntilDone(0.1f);
		Require(!scene.Replay->IsPlaying(), "Bullets_Setup_PlaybackFinished");
		Check(scene.Bullets.Contains(bulletId) && Near(CenterOf(*scene.Objects.Find(bulletId)).X, bulletX) &&
		          Near(scene.Bullets.All()[0].Age, bulletAge),
		      "Bullets_AfterPlayback_TheSameBulletWithTheSamePositionAndAge");
		scene.Frame(0.1f);
		Check(Near(CenterOf(*scene.Objects.Find(bulletId)).X, bulletX + 20.0f) && Near(scene.Bullets.All()[0].Age, bulletAge + 0.1f),
		      "Bullets_AfterPlayback_TheyResumeFlying");
		scene.Frame(1.5f);
		Check(scene.Bullets.All().empty(), "Bullets_AfterPlayback_TheyExpireNormally");
	}
	{
		Scene scene; // replayed bullets are exactly the recorded ones
		scene.StartShip();
		scene.RecordFor([&] { scene.Frame(0.1f, false, false, true); scene.Frame(0.5f); scene.Frame(0.5f); });
		Require(scene.Replay->Frames().size() == 4, "ReplayBullets_Setup"); // rel 0, 0.1, 0.6, 1.1
		scene.Frame(5.0f); // everything live is long gone
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(scene.Replay->CurrentSample().Bullets.empty(), "ReplayBullets_FirstFrame_NoBullet");
		scene.Frame(0.6f); // elapsed 0.6: the third sample; the bullet left the muzzle (516) at 0.1 and flew 0.6 s
		Check(scene.Replay->CurrentSample().Bullets.size() == 1 && Near(scene.Replay->CurrentSample().Bullets[0].Centre.X, 636.0f),
		      "ReplayBullets_ComeFromTheRecordedSample");
		Check(scene.Bullets.All().empty(), "ReplayBullets_NeverEnterTheLiveField");
	}

	// ======== Controller gates during playback ========
	{
		Scene scene;
		scene.StartShip();
		RecordStraightFlight(scene); // no input was recorded
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Seen.clear();
		std::size_t bulletsBefore = scene.Bullets.All().size();
		scene.Frame(0.1f, true, true, true);
		scene.Frame(0.1f, true, false, true);
		Check(scene.Count(Engine::EventType::Input) == 0, "Gate_LiveLeftRightFire_RaiseNoInputEventsDuringPlayback");
		Check(scene.Bullets.All().size() == bulletsBefore && scene.Sent.empty(), "Gate_LiveFire_CreatesNoBulletAndAnnouncesNothing");
		Check(Near(scene.Controller->Heading(), 0.0f), "Gate_LiveRotation_DoesNotTurnTheReplayedShip");
		Engine::Vector2 velocityBefore = scene.Ship().GetVelocity();
		scene.Controller->ApplySteering(0.5f);
		scene.Controller->WrapAroundArena();
		Check(scene.Ship().GetVelocity().X == velocityBefore.X && scene.Ship().GetVelocity().Y == velocityBefore.Y,
		      "Gate_Steering_DoesNotRunDuringPlayback");
		ShootShip(scene);
		std::size_t pending = scene.Events.PendingCount();
		scene.Controller->DetectBulletHits();
		Check(scene.Events.PendingCount() == pending, "Gate_LiveCollisionDetection_Disabled");
		Check(scene.Controller->IsPlaybackMode(), "Gate_PlaybackModeIsOn");
	}
	{
		Scene scene; // a replayed ActionPrimary creates no live bullet and announces nothing
		scene.StartShip();
		scene.RecordFor(
		    [&]
		    {
			    scene.Frame(0.1f, false, false, true);
			    scene.Frame(0.6f, false, false, true);
		    });
		scene.Frame(5.0f); // the recorded bullets are gone from the live field
		Require(scene.Bullets.All().empty(), "Gate_ReplayedInput_Setup_NoLiveBullets");
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Seen.clear();
		scene.Sent.clear();
		scene.Run(1.0f, 0.05f);
		Check(scene.Count(Engine::EventType::Input) >= 2, "Gate_ReplayedInputEventsAreRedispatched_Observed");
		Check(scene.Bullets.All().empty() && scene.Objects.Count() == 2, "Gate_ReplayedActionPrimary_CreatesNoLiveBullet");
		Check(scene.Sent.empty(), "Network_ReplayedShoot_NotAnnounced");
	}
	{
		Scene scene; // a replayed Death schedules no real Spawn; a replayed Spawn does not touch live state
		scene.StartShip();
		scene.Replay->RequestStartRecording();
		scene.Frame(0.0f);
		scene.Frame(1.0f);
		ShootShip(scene);
		scene.Frame(0.1f); // dies at rel 1.1
		scene.Frame(1.0f); // rel 2.1
		scene.Frame(1.1f); // respawn due at rel 3.1: Spawn recorded
		scene.Replay->RequestStopRecording();
		scene.Frame(0.0f);
		Require(scene.Controller->IsAlive(), "Gate_Death_Setup_AliveAfterTheRecordedRespawn");
		bool hasDeath = false, hasSpawn = false;
		for (const Engine::RecordedEvent& recorded : scene.Replay->RecordedEvents())
		{
			hasDeath = hasDeath || recorded.Payload.index() == static_cast<std::size_t>(Engine::EventType::Death);
			hasSpawn = hasSpawn || recorded.Payload.index() == static_cast<std::size_t>(Engine::EventType::Spawn);
		}
		Require(hasDeath && hasSpawn, "Gate_Death_Setup_RecordingHasDeathAndSpawn");

		scene.Frame(0.3f);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		ShipState liveState = scene.Controller->State();
		scene.Seen.clear();
		scene.Sent.clear();
		scene.Run(4.0f, 0.1f);
		Require(!scene.Replay->IsPlaying(), "Gate_Death_Setup_PlaybackFinished");
		Check(scene.Count(Engine::EventType::Death) == 1 && scene.Count(Engine::EventType::Spawn) == 1,
		      "Gate_ReplayedDeathAndSpawn_AreRedispatched");
		Check(scene.Events.PendingCount() == 0, "Gate_ReplayedDeath_ScheduledNoRealSpawn");
		Check(scene.Controller->IsAlive(), "Gate_ReplayedDeath_DidNotKillTheLiveShip");
		Check(scene.Controller->State().LastFireTime == liveState.LastFireTime, "Gate_ReplayedSpawn_LeftTheLiveStateAlone");
		Check(scene.Sent.empty(), "Network_ReplayedDeathAndSpawn_NotAnnounced");
		scene.Seen.clear();
		scene.Run(4.0f, 0.5f);
		Check(scene.Count(Engine::EventType::Spawn) == 0 && scene.Controller->IsAlive(), "Gate_NoStraySpawnAppearsAfterPlayback");
	}
	{
		Scene scene; // replayed Spawn never mutates the live invulnerability window
		scene.StartShip();
		scene.Replay->RequestStartRecording();
		scene.Frame(0.0f);
		ShootShip(scene);
		scene.Frame(0.0f);
		scene.Frame(2.25f); // the respawn (2 s after the death) comes due: a Spawn is recorded
		scene.Replay->RequestStopRecording();
		scene.Frame(0.0f);
		bool recordedSpawn = false;
		for (const Engine::RecordedEvent& recorded : scene.Replay->RecordedEvents())
		{
			recordedSpawn = recordedSpawn || recorded.Payload.index() == static_cast<std::size_t>(Engine::EventType::Spawn);
		}
		Require(recordedSpawn, "Gate_Spawn_Setup_TheRecordingContainsASpawn");
		scene.Frame(1.5f); // live protection from that live Spawn has run out
		Require(!scene.Controller->IsInvulnerable(), "Gate_Spawn_Setup_NotProtected");
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		double untilBefore = scene.Controller->State().InvulnerableUntil;
		scene.Seen.clear();
		bool leaked = false;
		int framesPlayed = 0;
		while (scene.Replay->IsPlaying() && framesPlayed++ < 1000)
		{
			scene.Frame(0.1f);
			// (the replayed Spawn runs in between; if it acted live it would protect the ship)
			leaked = leaked || scene.Controller->State().InvulnerableUntil > untilBefore;
		}
		Check(framesPlayed > 5 && !leaked && scene.Count(Engine::EventType::Spawn) == 1,
		      "Gate_ReplayedSpawn_RedispatchedButNeverTouchesTheLiveInvulnerabilityWindow");
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsInvulnerable(), "Gate_ReplayedSpawn_LiveShipStillUnprotectedAfterwards");
	}

	// ======== Network during playback ========
	{
		Scene scene;
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Frame(0.4f);
		scene.Bridge->HandleNetworkEvent(Scene::RemoteShoot(700.0f, 100.0f)); // before playback: a normal shot
		Check(scene.Bullets.All().size() == 1, "Remote_BeforePlayback_ShootIsRebuilt");
		scene.Frame(5.0f);
		Require(scene.Bullets.All().empty(), "Remote_Setup_FieldEmpty");
		ShipSnapshot live = scene.ShipObjectState();

		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		// The ship object shows the recording, but the state Game would publish is the saved live one.
		Check(Near(scene.Published.Position.X, live.Position.X) && Near(scene.Published.Position.Y, live.Position.Y) &&
		          Near(scene.Published.Velocity.X, live.Velocity.X),
		      "Network_PublishedStateIsTheSavedLiveState_NotTheReplay");
		Check(!Near(scene.ShipObjectState().Position.X, live.Position.X), "Network_TheReplayedShipObjectDiffersFromWhatIsPublished");

		scene.Incoming.push_back(Scene::RemoteShoot(700.0f, 100.0f));
		scene.Incoming.push_back(Scene::RemoteShoot(800.0f, 100.0f));
		scene.Frame(0.1f);
		Check(scene.Bullets.All().empty(), "RemoteShoot_DuringPlayback_BuildsNoBullet");
		Check(scene.Incoming.empty(), "RemoteShoot_DuringPlayback_WasDrainedFromTheIncomingQueue");
		Check(!scene.Bridge->HandleNetworkEvent(Scene::RemoteShoot(750.0f, 100.0f)), "RemoteShoot_DuringPlayback_IsDiscardedByTheBridge");

		// A remote ship's state keeps applying (Game writes it straight onto the remote object).
		SetCenter(*scene.Objects.Find(scene.RemoteId), { 1000.0f, 700.0f });
		scene.Frame(0.1f);
		Check(Near(CenterOf(*scene.Objects.Find(scene.RemoteId)).X, 1000.0f) && Near(CenterOf(*scene.Objects.Find(scene.RemoteId)).Y, 700.0f),
		      "RemoteState_ContinuesDuringPlayback_ReplayNeverTouchesRemoteShips");

		scene.Incoming.push_back(Scene::RemoteDeath());
		scene.Frame(0.1f);
		scene.Frame(0.0f);
		Check(!scene.Bridge->IsRemoteAlive(scene.RemoteId), "RemoteDeath_ContinuesDuringPlayback");
		scene.Incoming.push_back(Scene::RemoteSpawn());
		scene.Frame(0.1f);
		scene.Frame(0.0f);
		Check(scene.Bridge->IsRemoteAlive(scene.RemoteId), "RemoteSpawn_ContinuesDuringPlayback");
		Check(scene.Sent.empty(), "Network_NothingAtAllAnnouncedDuringPlayback");

		scene.RunUntilDone(0.1f);
		Require(!scene.Replay->IsPlaying(), "Remote_Setup_PlaybackFinished");
		scene.Frame(0.1f);
		scene.Frame(1.0f);
		Check(scene.Bullets.All().empty(), "RemoteShoot_DiscardedShotsNeverAppearAfterPlayback");

		scene.Incoming.push_back(Scene::RemoteShoot(900.0f, 100.0f));
		scene.Frame(0.0f);
		Check(scene.Bullets.All().size() == 1, "RemoteShoot_AfterPlayback_IsRebuiltAgain");
	}
	{
		Scene scene; // the announcement gate: all three kinds, then restored
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		scene.Bridge->AnnounceShot({ 1.0f, 2.0f }, 0.0f);
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Events.Raise(Engine::SpawnEventData{ scene.ShipId, { 5.0f, 5.0f } });
		scene.Frame(0.0f);
		Check(scene.Sent.empty(), "Gate_Announce_ShootDeathAndSpawnAllSuppressedDuringPlayback");
		scene.RunUntilDone(0.1f);
		Require(!scene.Replay->IsPlaying(), "Gate_Announce_PlaybackFinished");
		scene.Sent.clear();
		scene.Frame(0.0f, false, false, true);
		Check(scene.Sent.size() == 1 && scene.Sent[0].Kind == Engine::NetworkEventKind::Shoot, "Network_AnnounceRestoredAfterPlayback_Shoot");
		scene.Events.Raise(Engine::DeathEventData{ scene.ShipId, Engine::NoObjectId });
		scene.Frame(0.0f);
		Check(scene.Sent.size() == 2 && scene.Sent[1].Kind == Engine::NetworkEventKind::Death, "Network_AnnounceRestoredAfterPlayback_Death");
	}

	// ======== Playback only while the live ship is alive ========
	{
		Scene scene;
		scene.StartShip();
		RecordStraightFlight(scene);
		std::size_t frames = scene.Replay->Frames().size();
		ShootShip(scene);
		scene.Frame(0.0f); // the live ship dies; its own respawn is scheduled 2 s out
		Require(!scene.Controller->IsAlive() && scene.Events.PendingCount() == 1, "Dead_Setup_ShipDeadWithASpawnPending");

		scene.Replay->RequestPlayback(); // F7 while dead
		scene.Frame(0.0f);
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsPlaybackMode(), "Dead_F7_DoesNotStartPlayback");
		Check(scene.Replay->Frames().size() == frames, "Dead_F7_LeavesTheRecordingIntact");
		Check(scene.Events.PendingCount() == 1, "Dead_F7_RaisedNothing_PendingRespawnUntouched");
		scene.Frame(2.0f);
		Check(scene.Controller->IsAlive(), "Dead_TheLiveRespawnStillHappens");

		scene.Replay->RequestPlayback(); // alive again: now it works with the same recording
		scene.Frame(0.0f);
		Check(scene.Replay->IsPlaying(), "Dead_ThenAlive_TheSameRecordingPlays");
	}

	// ======== Completion ========
	{
		Scene scene;
		scene.StartShip();
		RecordStraightFlight(scene);
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(scene.Controller->IsPlaybackMode(), "Completion_Setup_PlaybackModeOn");
		scene.RunUntilDone(0.1f);
		Check(!scene.Replay->IsPlaying() && !scene.Controller->IsPlaybackMode(), "Completion_PlaybackModeClears");

		scene.Seen.clear();
		double headingBefore = scene.Controller->Heading();
		scene.Frame(0.25f, false, true, false);
		Check(scene.Count(Engine::EventType::Input) == 1 && scene.Controller->Heading() > headingBefore + 0.4f,
		      "Completion_LiveInputWorksAgain");
		scene.Frame(0.5f);
		Check(scene.Ship().GetVelocity().X != 0.0f || scene.Ship().GetVelocity().Y != 0.0f, "Completion_LiveShipFliesAgain");
		scene.Frame(0.0f, false, false, true);
		Check(!scene.Sent.empty() && scene.Sent.back().Kind == Engine::NetworkEventKind::Shoot, "Completion_NetworkAnnounceResumes");
		Check(scene.Replay->Frames().size() == 4, "Completion_TheRecordingIsKeptForAnotherPlayback");
		scene.Replay->RequestPlayback();
		scene.Frame(0.0f);
		Check(scene.Replay->IsPlaying(), "Completion_TheSameRecordingCanBePlayedAgain");
	}

	if (g_Failures == 0)
	{
		std::printf("All tests passed.\n");
		return 0;
	}

	std::printf("%d test(s) failed.\n", g_Failures);
	return 1;
}
