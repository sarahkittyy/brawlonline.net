// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

// Which side of a conditional branch a Jit64 block goes on with.
//
// A block ends where a conditional branch is taken and goes on where it is not
// (PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE). Game code is not laid out for that: in Brawl 57% of
// all block exits are conditional branches that were taken, most of them at branches that are
// taken nearly every time, and each such exit stores the block's registers, goes through a link
// and loads them again in the next block.
//
// Data/Sys/Rollback/jit-branches.txt lists the branches of Project+ that were taken at least 90%
// of the time in recorded play (tools/jit-branch-profile.py makes it). The analyzer follows
// those as it follows an unconditional branch, and not taken becomes the side exit. A line holds
// the branch's address and its instruction, so a line whose address holds other code (another
// game, another build of a module) does nothing, and a wrong line only costs what it would have
// saved: what a block computes does not change, only where blocks begin and end.
namespace BranchProfile
{
// A recording's counts of one conditional branch.
struct Counts
{
  u32 taken = 0;
  u32 not_taken = 0;
  u32 instruction = 0;
};

// A block follows at most this many taken branches.
constexpr u32 MAX_FOLLOWS_PER_BLOCK = 6;

// Loads the profile: PPR_JIT_BRANCH_PROFILE=<file> in place of the one in Sys, =0 for none.
void Init();
// Writes the recording, if there is one.
void Shutdown();

// False in a whole-machine session, a movie and netplay without rollback, where every machine
// (or every run) must take its interrupts at the same instructions, and without a profile.
bool IsActive();

// Analyzer: whether blocks go on at the target of the conditional branch `instruction` (bcx on a
// CR bit, without CTR and LK) at `address`.
bool FollowsTaken(u32 address, u32 instruction);

// Recording (PPR_JIT_BRANCH_RECORD=<file>): no branch is followed, and Jit64 counts the two
// sides of every such branch here; Shutdown writes the counts. nullptr when not recording.
Counts* Recording(u32 address, u32 instruction);
}  // namespace BranchProfile
