// Sprite decoder and write-log observer for builds whose game has no
// games/<game>/video/.
//
// Such games never instantiate GameVideo (the frontend requires F3RT_GAME_VIDEO),
// but runtime/renderer/game/video.cpp is always linked, so the entry points must
// resolve. These bodies are unreachable in a correctly configured build.
#include "renderer/game/sprites.hpp"
#ifdef F3RT_VIDEO_WRITE_LOG
#include "renderer/game/video_log.hpp"
#endif

namespace f3rt {

void GameSprites::decode(const VideoRam &) {}
#ifdef F3RT_VIDEO_WRITE_LOG
void observe_game_video_write(uint32_t, uint32_t, uint64_t) {}
#endif

} // namespace f3rt
