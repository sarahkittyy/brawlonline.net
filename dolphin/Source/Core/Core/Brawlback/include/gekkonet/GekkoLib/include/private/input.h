#pragma once

#include "gekkonet.h"
#include "gekko_types.h"
#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace Gekko {
	struct GameInput {
	public:
		GameInput();

		void Init(GameInput* other);

		void Init(Frame frame_num, u8* inp, u32 inp_len);

		bool IsEqualTo(u8* other);

		void Clear();

	public:
		static const i32 NULL_FRAME = -1;

		Frame frame;

		std::unique_ptr<u8[]> input;

		u32 input_len;
	};

	struct InputBuffer {
		static const u32 DEFAULT_BUFF_SIZE = 128;

		InputBuffer();

		void Init(u8 delay, u8 input_window, u32 input_size, u32 buffer_size = DEFAULT_BUFF_SIZE);

		void AddLocalInput(Frame frame, u8* input);

		void AddInput(Frame frame, u8* input);

		// a misprediction of `frame` alone is forgotten (the frame had not used the input yet).
		bool AcceptLateInput(Frame frame);

		void OverwriteInput(Frame frame, u8* input);

		void SetDelay(u8 delay);

		// the input of the frames the local delay holds back at the start (zeroes by default).
		void SetDelayFillInput(const u8* input);

		u8 GetDelay();
		
		void SetInputPredictionWindow(u8 input_window);

		Frame GetIncorrectPredictionFrame();

		std::unique_ptr<GameInput> GetInput(Frame frame, bool prediction = false);

		void SetRunaheadMode(bool running_ahead);

		Frame GetLastReceivedFrame();

        void ClearIncorrectFrames(Frame clear_limit);

		// input suggestions (a remote player's, gekko_add_input_suggestion): its input of `frame`
		// and later frames unless it changes again; predictions use the newest one (highest `seq`).
		void AddSuggestion(Frame frame, u32 seq, const u8* input);

		const GekkoSuggestionStats& GetSuggestionStats() const { return _sugg_stats; }

	private:
		void ResetPrediction();

		u32 PreviousFrame(Frame frame);

		bool HandleInputPrediction(Frame frame);

		bool CanPredictInput();

		// predicts `frame` (its slot) when the last input received is that of frame `last`: the
		// newest suggestion for a frame after `last` and no later than `frame`, else `last`'s input.
		// `first`: the frame's first prediction (the one the screen shows), not a refill.
		void Predict(Frame frame, Frame last, bool first);

		// frame `frame`'s input (an empty one for NULL_FRAME).
		const u8* InputOf(Frame frame) const;

	private:
		struct Suggestion {
			Frame frame = GameInput::NULL_FRAME;
			u32 seq = 0;
			std::vector<u8> input;
		};
		std::array<Suggestion, 8> _suggestions;

		// per slot: the frame's first prediction (the stats' shown_wrong).
		std::vector<u8> _shown;

		GekkoSuggestionStats _sugg_stats{};

		// the judge's bucket (gekko_set_suggestion_judge), and whether it stopped this player's
		// suggestions.
		u32 _worse_bucket = 0;
		bool _sugg_ignored = false;

		bool _running_ahead;

		u8 _input_delay;

		u8 _input_prediction_window;

		u32 _input_size;

		u32 _buff_size;

        std::unique_ptr<u8[]> _empty_input;

        std::unique_ptr<u8[]> _delay_fill_input;

		Frame _last_received_input;

		Frame _last_predicted_input;

		Frame _first_predicted_input;

		std::deque<Frame> _incorrent_predicted_inputs;

		std::deque<std::unique_ptr<GameInput>> _inputs;
	};
}
