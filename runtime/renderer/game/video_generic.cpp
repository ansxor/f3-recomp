// Write-log observer for builds whose game has no
// games/<game>/video/.
//
// Such games never instantiate GameVideo (the frontend requires F3RT_GAME_VIDEO),
// but runtime/renderer/game/video.cpp is always linked, so the entry point must
// resolve. This body is unreachable in a correctly configured build.
#ifdef F3RT_VIDEO_WRITE_LOG
#include "renderer/game/video_log.hpp"
#endif

namespace f3rt {

#ifdef F3RT_VIDEO_WRITE_LOG
void observe_game_video_write(uint32_t, uint32_t, uint64_t) {}
#endif

} // namespace f3rt
