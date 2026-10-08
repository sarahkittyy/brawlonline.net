#pragma once

#include <vector>

#include "gekko_types.h"
#include "gekkonet.h"
#include "storage.h"
#include "sync.h"

namespace Gekko {
    struct ReplayBlob {
        static constexpr u32 MAGIC = 0x474B5250; // GKRP
        static constexpr u32 FORMAT_VERSION = 1;

        u32 header = MAGIC;
        u32 version = FORMAT_VERSION;
        bool compressed = false;

        GekkoConfig config = {};

        std::vector<u8> inputs;
        std::vector<u8> initial_state;
    };

    struct ReplaySystem {
        bool StartRecording(GekkoConfig config, Frame frame, bool save_state, bool disable_compression);
        const u8* StopRecording(u32& length);
        void RecordInputs(SyncSystem& sync);
        void RecordState(const u8* state, u32 length, Frame frame);

        bool NeedsState();
        StateEntry* PendingState();

        bool LoadReplay(const u8* replay_data, u32 length);
        const u8* ReplayState();
        bool NextReplayInput(u8* input);

        GekkoConfig Config();

        bool IsRecording() const;
        bool IsReplaying() const;

        void Reset();

    private:

        void RecordInput(Frame frame, const u8* input);

        void RecordPendingState();

        u32 InputSize() const;

        enum Mode {
            None,
            Recording,
            Replaying
        } _mode = None;

        bool _needs_state = false;
        bool _pending_state = false;
        bool _no_compression = false;

        Frame _start_frame = 0;
        Frame _current_frame = 0;
        Frame _last_recorded_frame = GameInput::NULL_FRAME;

        std::vector<u8> _bin_buffer;

        StateEntry _state;

        ReplayBlob _replay;
    };
}
