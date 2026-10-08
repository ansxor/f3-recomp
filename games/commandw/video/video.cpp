// Command War (commandw) game-specific video code.
//
// - video_writer_known: no store-PC lists are documented for this game yet, so
//   `--discovery-log` reports every non-sprite writer (sprite RAM is covered by the
//   emit units and [video.frame_writers] in config.toml).
//
// The sprite, tile, text and line-RAM decoders are shared and live in runtime/.
#include "discovery_log.hpp"

namespace f3rt {

bool video_writer_known(VideoLayer, uint32_t) { return false; }

} // namespace f3rt
