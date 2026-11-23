//! Window scaling and blit placement.

use crate::config::{GameVideoOptions, Geometry, NATIVE_WIDTH, VideoScaleMode};

/// Scale factor that fits a `pixel_width` x `pixel_height` window.
pub fn video_scale_for_window(
    mode: VideoScaleMode,
    mut pixel_width: u32,
    mut pixel_height: u32,
    border: u32,
    portrait: bool,
    geometry: &Geometry,
) -> u32 {
    if portrait {
        std::mem::swap(&mut pixel_width, &mut pixel_height);
    }
    let native_width = NATIVE_WIDTH + border * 2;
    let scale = if mode == VideoScaleMode::Auto {
        // Ceil the limiting axis without floating point or overflowing an addition.
        let width_scale = pixel_width.div_ceil(native_width);
        let height_scale = pixel_height.div_ceil(geometry.height());
        width_scale.min(height_scale)
    } else {
        (pixel_width / native_width).min(pixel_height / geometry.height())
    };
    scale.clamp(1, GameVideoOptions::MAX_SCALE)
}

/// Source rectangle and destination rectangle for presenting the scene.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VideoBlit {
    pub source_x: u32,
    pub source_y: u32,
    pub source_width: u32,
    pub source_height: u32,
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

/// Blit rectangle for a `pixel_width` x `pixel_height` window.
pub fn video_blit_for_window(
    mode: VideoScaleMode,
    options: GameVideoOptions,
    pixel_width: u32,
    pixel_height: u32,
    portrait: bool,
    geometry: &Geometry,
) -> VideoBlit {
    let source_width = if portrait {
        options.height(geometry)
    } else {
        options.width(geometry)
    };
    let source_height = if portrait {
        options.width(geometry)
    } else {
        options.height(geometry)
    };
    if mode == VideoScaleMode::AutoInteger {
        let width = pixel_width.min(source_width);
        let height = pixel_height.min(source_height);
        return VideoBlit {
            source_x: (source_width - width) / 2,
            source_y: (source_height - height) / 2,
            source_width: width,
            source_height: height,
            x: (pixel_width - width) / 2,
            y: (pixel_height - height) / 2,
            width,
            height,
        };
    }
    let mut width = pixel_width;
    let mut height = pixel_height;
    if u64::from(pixel_width) * u64::from(source_height)
        > u64::from(pixel_height) * u64::from(source_width)
    {
        width =
            (u64::from(pixel_height) * u64::from(source_width) / u64::from(source_height)) as u32;
    } else {
        height =
            (u64::from(pixel_width) * u64::from(source_height) / u64::from(source_width)) as u32;
    }
    VideoBlit {
        source_x: 0,
        source_y: 0,
        source_width,
        source_height,
        x: (pixel_width - width) / 2,
        y: (pixel_height - height) / 2,
        width,
        height,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn geometry() -> Geometry {
        Geometry::default()
    }

    #[test]
    fn fixed_scale_truncates_and_clamps() {
        let g = geometry();
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Fixed, 640, 464, 0, false, &g),
            2
        );
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Fixed, 100, 100, 0, false, &g),
            1,
            "clamped to at least one"
        );
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Fixed, 100_000, 100_000, 0, false, &g),
            GameVideoOptions::MAX_SCALE
        );
    }

    #[test]
    fn auto_ceils_the_limiting_axis() {
        let g = geometry();
        // 321/320 ceil = 2, 233/232 ceil = 2 -> 2.
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Auto, 321, 233, 0, false, &g),
            2
        );
        // Height limits: 640/320=2, 463/232 ceil=2 -> 2; 464 gives floor 2 too.
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Auto, 640, 463, 0, false, &g),
            2
        );
        // Border widens the native width: 640 / (320 + 2) = 1.99 -> floor 1, ceil 2.
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Fixed, 640, 464, 1, false, &g),
            1
        );
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Auto, 640, 464, 1, false, &g),
            2
        );
    }

    #[test]
    fn portrait_swaps_window_axes() {
        let g = geometry();
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Fixed, 464, 640, 0, true, &g),
            2
        );
        assert_eq!(
            video_scale_for_window(VideoScaleMode::Fixed, 464, 640, 0, false, &g),
            1
        );
    }

    #[test]
    fn auto_integer_blits_without_scaling() {
        let g = geometry();
        let options = GameVideoOptions {
            scale: 2,
            border: 0,
        };
        let blit =
            video_blit_for_window(VideoScaleMode::AutoInteger, options, 1000, 700, false, &g);
        assert_eq!(
            blit,
            VideoBlit {
                source_x: 0,
                source_y: 0,
                source_width: 640,
                source_height: 464,
                x: 180,
                y: 118,
                width: 640,
                height: 464,
            }
        );
    }

    #[test]
    fn aspect_fit_letterboxes() {
        let g = geometry();
        let options = GameVideoOptions {
            scale: 1,
            border: 0,
        };
        let wide = video_blit_for_window(VideoScaleMode::Fixed, options, 1000, 232, false, &g);
        assert_eq!(wide.source_width, 320);
        assert_eq!(wide.source_height, 232);
        assert_eq!(wide.width, 320);
        assert_eq!(wide.height, 232);
        assert_eq!(wide.x, 340);
        assert_eq!(wide.y, 0);

        let tall = video_blit_for_window(VideoScaleMode::Fixed, options, 320, 1000, false, &g);
        assert_eq!(tall.width, 320);
        assert_eq!(tall.height, 232);
        assert_eq!(tall.y, 384);
    }

    #[test]
    fn portrait_swaps_source_dimensions() {
        let g = geometry();
        let options = GameVideoOptions {
            scale: 1,
            border: 0,
        };
        let blit = video_blit_for_window(VideoScaleMode::AutoInteger, options, 500, 400, true, &g);
        assert_eq!(blit.source_width, 232);
        assert_eq!(blit.source_height, 320);
        assert_eq!(blit.width, 232);
        assert_eq!(blit.height, 320);
    }
}
