#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace f3rt {

class SpriteGroupTracker;
struct SceneSprite;

enum class VideoBackend { Cpu, Gpu };

struct SpriteFrame {
    const SpriteGroupTracker *groups;
    uint16_t game_frame;
    bool flipped;

    static std::optional<SpriteFrame> read(const SpriteGroupTracker &groups, bool flipped);
};

class SpriteStage {
public:
    virtual ~SpriteStage() = default;
    virtual void present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) = 0;
    virtual void reset() = 0;
    virtual void report(std::ostream &out) const = 0;
};

class RenderEnhancements {
public:
    RenderEnhancements();
    explicit RenderEnhancements(std::vector<std::unique_ptr<SpriteStage>> stages);
    RenderEnhancements(RenderEnhancements &&) noexcept;
    RenderEnhancements &operator=(RenderEnhancements &&) noexcept;
    ~RenderEnhancements();

    bool empty() const { return m_stages.empty(); }
    void present(std::span<const SceneSprite> latched, std::optional<SpriteFrame> frame);
    std::span<const SceneSprite> sprites() const;
    void reset();
    void report(std::ostream &out) const;

private:
    std::vector<std::unique_ptr<SpriteStage>> m_stages;
    std::vector<SceneSprite> m_presented;
};

}
