#include "session/replay_session.h"

#include <cstdio>
#include <cstring>

Gekko::ReplaySession::ReplaySession()
{
    _started = false;
    _finished = false;
    _inputs = nullptr;
    _config = GekkoConfig();
}

void Gekko::ReplaySession::Init(GekkoConfig* config)
{
    // no-op: a replay session is configured by the replay it loads.
}

GekkoGameEvent** Gekko::ReplaySession::UpdateSession(i32* count)
{
    _session_events.Reset();

    _game_events.Clear();

    if (_replay.IsReplaying()) {
        _game_events.Reset();

        if (!_started) {
            _started = true;
            _session_events.AddSessionStartedEvent();
            AddInitialStateLoad();
        }

        if (AddNextReplayInputs()) {
            if (_game_events.AddAdvanceEvent(_sync, false)) {
                _sync.IncrementFrame();
            }
        }
        else if (!_finished) {
            _finished = true;
            _session_events.AddReplayFinishedEvent();
        }
    }

    *count = _game_events.Count();
    return _game_events.Data();
}

GekkoSessionEvent** Gekko::ReplaySession::Events(i32* count)
{
    *count = (i32)_session_events.GetRecentEvents().size();
    return _session_events.GetRecentEvents().data();
}

bool Gekko::ReplaySession::LoadReplay(const u8* replay_data, u32 length)
{
    if (!_replay.LoadReplay(replay_data, length)) {
        return false;
    }

    _config = _replay.Config();

    _started = false;
    _finished = false;

    try {
        _sync.Init(_config.num_players, _config.input_size);

        _game_events.Init(_config.input_size * _config.num_players);

        _inputs = std::make_unique<u8[]>(_config.input_size * _config.num_players);
    }
    catch (...) {
        printf("replay does not fit in memory\n");
        _replay.Reset();
        return false;
    }

    _session_events.Reset();

    return true;
}

void Gekko::ReplaySession::AddInitialStateLoad()
{
    const u8* state = _replay.ReplayState();

    if (!state) {
        return;
    }

    _game_events.AddStateLoadEvent(_sync.GetCurrentFrame() - 1, (u8*)state, _config.state_size);
}

bool Gekko::ReplaySession::AddNextReplayInputs()
{
    if (!_inputs || !_replay.NextReplayInput(_inputs.get())) {
        return false;
    }

    const Frame frame = _sync.GetCurrentFrame();

    for (u8 player = 0; player < _config.num_players; player++) {
        _sync.AddRemoteInput(player, _inputs.get() + (player * _config.input_size), frame);
    }

    return true;
}
