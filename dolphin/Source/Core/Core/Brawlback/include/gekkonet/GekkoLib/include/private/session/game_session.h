#pragma once

#include <memory>

#include "gekkonet.h"
#include "gekko_types.h"
#include "session.h"
#include "backend.h"
#include "event.h"
#include "replay.h"
#include "sync.h"
#include "storage.h"

namespace Gekko {

	class GameSession : public GekkoSession {
    public:
        GameSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        void SetRunahead(u8 runahead) override;

        void SetNetAdapter(GekkoNetAdapter* adapter) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        bool DisconnectActor(i32 actor) override;

        void SetDisconnectTimeout(u32 timeout) override;

        void SetDisconnectedInput(const void* input) override;

        void AddLocalInput(i32 player, void* input) override;

        bool AddLocalInputAt(i32 player, void* input, i32 frame) override;

        void SendLocalInputsNow() override;

        i32 LocalInputFrame(i32 player) override;

        i32 CurrentFrame() override;

        void ReceiveOnly() override;

        bool ReceivedInput(i32 player, i32 frame, void* input) override;

        bool AcceptLateInput(i32 player, i32 frame) override;

        void SetAdvantageDelayBias(i32 frames) override;

        void SetFrameLimit(i32 frame) override;

        void SetLocalDelayInput(i32 player, const void* input) override;

        i32 ConfirmedFrame() override;

        i32 RollbackFrames() override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        f32 FramesAhead() override;

        void NetworkStats(i32 player, GekkoNetworkStats* stats) override;

        void NetworkPoll() override;

        bool StartRecording(bool save_initial_state, bool disable_compression) override;

        const u8* StopRecording(u32& length) override;

	private:
		void Poll();

        void UpdateRecording();

        void RecordInitialState();

		bool AllActorsValid();

		void HandleReceivedInputs();

		void SendLocalInputs();

		// moves the local inputs not in the network queue yet into it; true if any moved.
		bool QueueLocalInputs();

		// the first frame SendLocalInputsNow queued whose frame advantage is not recorded yet.
		Frame _adv_unrecorded_from = GameInput::NULL_FRAME;

		// gekko_set_advantage_delay_bias
		Frame _adv_delay_bias = 0;

		u8 GetMinLocalDelay();

		bool IsPlayingLocally();

        bool IsLockstepActive() const;

		void AddDisconnectedPlayerInputs();

		void SendSpectatorInputs();

		void HandleRollback();

		void HandleSavingConfirmedFrame();

		void HandleRunahead();

		void RewindRunahead();

		bool RollbackPending();

		bool ConfirmedSaveDue();

		Frame GetConfirmedFrame();

		bool ShouldStallAdvance();

        void SendSessionHealthCheck();

        void SendNetworkHealthCheck();

        void SessionIntegrityCheck();

	private:
		bool _started;

		Frame _last_saved_frame;

        Frame _last_sent_healthcheck;

		Frame _runahead_start_frame;

		u8 _runahead_frames;

		// the session does not advance this frame or later ones (INT32_MAX: no limit).
		Frame _frame_limit;

		std::unique_ptr<u8[]> _disconnected_input;

		GekkoConfig _config;

		SyncSystem _sync;

        GekkoNetAdapter* _host;

		MessageSystem _msg;

		StateStorage _storage;

        GameEventSystem _game_events;

        ReplaySystem _replay;
	};
}
