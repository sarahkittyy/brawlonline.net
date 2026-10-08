#pragma once

#include <vector>
#include <map>

#include "gekkonet.h"
#include "gekko_types.h"
#include "session.h"
#include "backend.h"
#include "event.h"
#include "replay.h"
#include "sync.h"
#include "storage.h"

namespace Gekko {

    class StressSession : public GekkoSession {
    public:
        StressSession();

        void Init(GekkoConfig* config) override;

        void SetLocalDelay(i32 player, u8 delay) override;

        i32 AddActor(GekkoPlayerType type, GekkoNetAddress* addr) override;

        void AddLocalInput(i32 player, void* input) override;

        GekkoGameEvent** UpdateSession(i32* count) override;

        GekkoSessionEvent** Events(i32* count) override;

        bool StartRecording(bool save_initial_state, bool disable_compression) override;

        const u8* StopRecording(u32& length) override;

    private:
        void HandleRollback();

        void CheckForDesyncs(Frame check_frame);

        void UpdateRecording();

    private:
        GekkoConfig _config;

        SyncSystem _sync;

        StateStorage _storage;

        SessionEventSystem _session_events;

        GameEventSystem _game_events;

        ReplaySystem _replay;

        std::vector<Player> _locals;

        u32 _check_distance;

        std::map<Frame, u32> _checksum_history;
    };
}
