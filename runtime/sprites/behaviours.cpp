#include "sprites/behaviour.hpp"
#include <array>
#ifdef F3RT_SPRITE_BEHAVIOURS_HEADER
#include F3RT_SPRITE_BEHAVIOURS_HEADER
#endif

namespace f3rt {
std::span<const SpriteBehaviour *const> registered_sprite_behaviours() {
#ifdef F3RT_SPRITE_BEHAVIOURS_HEADER
    return std::span<const SpriteBehaviour *const>(game_sprites::behaviours);
#else
    return {};
#endif
}
std::span<const FlickerShadow *const> registered_flicker_shadows() {
#ifdef F3RT_SPRITE_BEHAVIOURS_HEADER
    return std::span<const FlickerShadow *const>(game_sprites::flicker_shadows);
#else
    return {};
#endif
}
} // namespace f3rt
