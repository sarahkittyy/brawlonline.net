// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace HLE_Misc
{
bool IsResimulationPass();
void UnimplementedFunction(const Core::CPUThreadGuard& guard);
void HBReload(const Core::CPUThreadGuard& guard);
void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard);
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetUnconditionalFrame(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetFrameEnd(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetLoopEnd(const Core::CPUThreadGuard& guard);
void BrawlbackDVDCancelSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackCancelTaskSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackGXDrawDoneSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackDVDReadPrioSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackVIWaitForRetraceSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackFrameBufferSyncWaitHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipPadThreadReadHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimRenderHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimTaskProcessFirstHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimTaskProcessSecondHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimWaveSoundAllocHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimSeqSoundAllocHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimStrmSoundAllocHook(const Core::CPUThreadGuard& guard);
void GprbSoundAttachHook(const Core::CPUThreadGuard& guard);
void BrawlbackSyncCharSelectRandomSeedHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimPadThreadReadHook(const Core::CPUThreadGuard& guard);
void BrawlbackCapturePadThreadReadHook(const Core::CPUThreadGuard& guard);
void GprbPadThreadLoopHook(const Core::CPUThreadGuard& guard);
void GprbPacingElapsedHook(const Core::CPUThreadGuard& guard);
void GprbPacingStepsHook(const Core::CPUThreadGuard& guard);
void GprbRngTraceHook(const Core::CPUThreadGuard& guard);
void GprbProbeHook(const Core::CPUThreadGuard& guard);
}  // namespace HLE_Misc
