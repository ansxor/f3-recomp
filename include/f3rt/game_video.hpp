#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>

namespace f3rt {
class Machine;

class GameVideo {
public:
    explicit GameVideo(Machine &machine);
    ~GameVideo();
    GameVideo(const GameVideo &) = delete;
    GameVideo &operator=(const GameVideo &) = delete;
    void reset();
    void observe();
    void observe_write(uint32_t pc, uint32_t address);
    void compare_playfields(uint64_t frame, unsigned layer_mask);
    void report(std::ostream &output) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
