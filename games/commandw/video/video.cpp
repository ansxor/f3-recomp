// Command War (commandw) game-specific video code.
//
// - video_writer_known: no store-PC lists are documented for this game yet, so
//   `--discovery-log` reports every writer, sprite RAM included (group keys are derived
//   from the generated sprite-group table, which needs no writer list).
//
// The sprite, tile, text and line-RAM decoders are shared and live in runtime/.
#include "debug/discovery.hpp"

namespace f3rt {

bool video_writer_known(VideoLayer, uint32_t) { return false; }

} // namespace f3rt
