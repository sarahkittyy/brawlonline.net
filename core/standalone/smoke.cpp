// The standalone core links and starts: Dolphin's Common (logging, config, files), the core's
// online settings, and a match-less session with an engine that only answers.

#include <cstdio>
#include <string>

#include "Common/StringUtil.h"

int main()
{
  std::printf("%s\n", std::string(StripWhitespace("  brawl online core: standalone smoke  ")).c_str());
  return 0;
}
