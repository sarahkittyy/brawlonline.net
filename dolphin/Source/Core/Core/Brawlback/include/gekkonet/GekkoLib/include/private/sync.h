#pragma once

#include <vector>

#include "gekko_types.h"
#include "input.h"

namespace Gekko {

	class SyncSystem {
	public:
		SyncSystem();

		void Init(u8 num_players, u32 input_size, u32 buffer_size = InputBuffer::DEFAULT_BUFF_SIZE);

		void AddLocalInput(Handle player, u8* input);

		bool AddLocalInputAt(Handle player, u8* input, Frame frame);

		Frame GetLastLocalInput(Handle player);

		bool GetReceivedInput(Handle player, Frame frame, u8* input);

		bool AcceptLateInput(Handle player, Frame frame);

		void AddRemoteInput(Handle player, u8* input, Frame frame);

		void OverwriteInput(Handle player, u8* input, Frame frame);

		void IncrementFrame();

		bool GetCurrentInputs(std::unique_ptr<u8[]>& inputs, Frame& frame);

		void SetRunaheadMode(bool running_ahead);

		bool GetSpectatorInputs(std::unique_ptr<u8[]>& inputs, Frame frame);

		bool GetLocalInput(Handle player, std::unique_ptr<u8[]>& input, Frame frame);

		void SetLocalDelay(Handle player, u8 delay);

		void SetDelayFillInput(Handle player, const u8* input);
		
		u8 GetLocalDelay(Handle player);

		void SetInputPredictionWindow(Handle player, u8 input_window);

		Frame GetCurrentFrame() const;

		void SetCurrentFrame(Frame frame);

		Frame GetMinIncorrectFrame();

		Frame GetMinReceivedFrame();

        Frame GetLastReceivedFrom(Handle player);

        void ClearIncorrectFramesUpTo(Frame clear_limit);

	private:
		u8 _num_players;

		u32 _input_size;

		Frame _current_frame;

		std::unique_ptr<InputBuffer[]> _input_buffers;
	};
}
