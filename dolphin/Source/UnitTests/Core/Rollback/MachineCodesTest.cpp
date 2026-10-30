// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A room's codes as the launcher gives them (Machine::ParseCodes, docs/design/room-codes.md):
// Gecko and Action Replay lines into the session's options, anything else refused.

#include <string>

#include <gtest/gtest.h>
#include <picojson.h>

#include "Core/Rollback/MachineSession.h"

namespace
{
bool Parse(const std::string& json, Machine::Options* opts, std::string* error)
{
  picojson::value v;
  const std::string err = picojson::parse(v, json);
  EXPECT_TRUE(err.empty()) << err;
  return Machine::ParseCodes(v, opts, error);
}
}  // namespace

TEST(MachineCodes, GeckoAndActionReplay)
{
  Machine::Options opts;
  std::string error;
  ASSERT_TRUE(Parse(R"([
    {"name": "QOL - Faster Boot Time", "kind": "gecko",
     "lines": ["204cdc74 386003e8", " 044cdc34 48000038 ", "", "E2000001 80008000"]},
    {"name": "Marker", "kind": "ar", "lines": ["04002F04 CAFEBABE"]}])",
                    &opts, &error))
      << error;
  ASSERT_EQ(opts.gecko_codes.size(), 1u);
  const Gecko::GeckoCode& g = opts.gecko_codes[0];
  EXPECT_EQ(g.name, "QOL - Faster Boot Time");
  EXPECT_TRUE(g.enabled);
  // Blank lines left out, the rest trimmed.
  ASSERT_EQ(g.codes.size(), 3u);
  EXPECT_EQ(g.codes[0].address, 0x204cdc74u);
  EXPECT_EQ(g.codes[1].data, 0x48000038u);
  EXPECT_EQ(g.codes[2].address, 0xE2000001u);
  ASSERT_EQ(opts.ar_codes.size(), 1u);
  EXPECT_EQ(opts.ar_codes[0].name, "Marker");
  ASSERT_EQ(opts.ar_codes[0].ops.size(), 1u);
  EXPECT_EQ(opts.ar_codes[0].ops[0].cmd_addr, 0x04002F04u);
  EXPECT_EQ(opts.ar_codes[0].ops[0].value, 0xCAFEBABEu);
}

TEST(MachineCodes, EmptyListIsNoCodes)
{
  Machine::Options opts;
  std::string error;
  ASSERT_TRUE(Parse("[]", &opts, &error)) << error;
  EXPECT_TRUE(opts.gecko_codes.empty());
  EXPECT_TRUE(opts.ar_codes.empty());
}

TEST(MachineCodes, RefusesWhatIsNotACode)
{
  const char* bad[] = {
      R"({"name": "x"})",
      R"([{"name": "x", "kind": "gecko"}])",
      R"([{"name": "x", "kind": "gecko", "lines": []}])",
      R"([{"name": "x", "kind": "gecko", "lines": ["  "]}])",
      R"([{"name": "x", "kind": "gecko", "lines": [7]}])",
      R"([{"name": "x", "kind": "gecko", "lines": ["not a code"]}])",
      R"([{"name": "x", "kind": "ar", "lines": ["04002F04"]}])",
      R"([{"name": "x", "kind": "patch", "lines": ["04002F04 00000001"]}])",
      R"([{"kind": "gecko", "lines": ["04002F04 00000001"]}])",
      R"(["04002F04 00000001"])",
  };
  for (const char* json : bad)
  {
    Machine::Options opts;
    std::string error;
    EXPECT_FALSE(Parse(json, &opts, &error)) << json;
    EXPECT_FALSE(error.empty()) << json;
  }
}

TEST(MachineCodes, Bounded)
{
  // More lines than a session takes (2048 in all).
  std::string many = R"([{"name": "x", "kind": "gecko", "lines": [)";
  for (int i = 0; i < 2049; ++i)
    many += std::string(i ? "," : "") + "\"04002F04 00000001\"";
  many += "]}]";
  Machine::Options opts;
  std::string error;
  EXPECT_FALSE(Parse(many, &opts, &error));
  // More codes than a session takes (256).
  std::string codes = "[";
  for (int i = 0; i < 257; ++i)
    codes += std::string(i ? "," : "") + R"({"name": "x", "kind": "gecko", "lines": ["04002F04 00000001"]})";
  codes += "]";
  EXPECT_FALSE(Parse(codes, &opts, &error));
  // A long name is cut short.
  const std::string name(300, 'n');
  ASSERT_TRUE(Parse(R"([{"name": ")" + name + R"(", "kind": "gecko", "lines": ["04002F04 00000001"]}])",
                    &opts, &error));
  EXPECT_EQ(opts.gecko_codes[0].name.size(), 128u);
}
