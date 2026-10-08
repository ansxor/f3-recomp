// Known-writer list for games without games/<game>/video/.
//
// Such games never instantiate GameVideo (the frontend requires F3RT_GAME_VIDEO), but
// `--discovery-log` still classifies their graphics/control stores, so the entry point
// must resolve: no store PC has a documented producer, every writer is reported.
#include "discovery_log.hpp"

namespace f3rt {

bool video_writer_known(VideoLayer, uint32_t) { return false; }

} // namespace f3rt
