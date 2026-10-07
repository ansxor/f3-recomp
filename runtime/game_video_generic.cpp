// Scene decoders for builds whose game has no games/<game>/video/.
//
// Such games never instantiate GameVideo (the frontend requires F3RT_GAME_VIDEO),
// but runtime/game_video.cpp is always linked, so the component entry points must
// resolve. These bodies are unreachable in a correctly configured build.
#include "game_tiles.hpp"
#include "game_text.hpp"
#include "game_sprites.hpp"
#include "game_lines.hpp"
#ifdef F3RT_VIDEO_WRITE_LOG
#include "game_video_log.hpp"
#endif

namespace f3rt {

void GameTiles::decode(const VideoRam &) {}
void GameText::decode(const VideoRam &) {}
void GameSprites::decode(const VideoRam &) {}
void GameLines::decode(const VideoRam &) {}
#ifdef F3RT_VIDEO_WRITE_LOG
void observe_game_video_write(uint32_t, uint32_t, uint64_t) {}
#endif

} // namespace f3rt
