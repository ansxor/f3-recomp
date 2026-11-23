//! Canonical save-state records encode fields individually, in native byte order and without padding.

use std::mem::size_of;

pub const WRITER_OVERFLOW: &str = "StateWriter buffer overflow";
pub const READER_UNDERFLOW: &str = "StateReader buffer underflow";

/// `require_state` message for a field outside its canonical range.
pub fn canonical_error(field: &str) -> String {
    format!("Snapshot field outside canonical representation: {field}")
}

/// Bounds-checked little-endian writer over caller-provided memory.
pub struct StateWriter<'a> {
    dst: &'a mut [u8],
    pos: usize,
}

impl<'a> StateWriter<'a> {
    pub fn new(dst: &'a mut [u8]) -> Self {
        Self { dst, pos: 0 }
    }

    fn reserve(&mut self, size: usize) -> Result<&mut [u8], String> {
        if self.remaining() < size {
            return Err(WRITER_OVERFLOW.to_string());
        }
        let at = self.pos;
        self.pos += size;
        Ok(&mut self.dst[at..at + size])
    }

    pub fn write_bytes(&mut self, src: &[u8]) -> Result<(), String> {
        if src.is_empty() {
            return Ok(());
        }
        self.reserve(src.len())?.copy_from_slice(src);
        Ok(())
    }

    pub fn write_u8(&mut self, value: u8) -> Result<(), String> {
        self.write_bytes(&[value])
    }

    pub fn write_u16(&mut self, value: u16) -> Result<(), String> {
        self.write_bytes(&value.to_le_bytes())
    }

    pub fn write_i16(&mut self, value: i16) -> Result<(), String> {
        self.write_bytes(&value.to_le_bytes())
    }

    pub fn write_u32(&mut self, value: u32) -> Result<(), String> {
        self.write_bytes(&value.to_le_bytes())
    }

    pub fn write_i32(&mut self, value: i32) -> Result<(), String> {
        self.write_bytes(&value.to_le_bytes())
    }

    pub fn advance(&mut self, size: usize) -> Result<(), String> {
        self.reserve(size).map(|_| ())
    }

    pub fn remaining(&self) -> usize {
        self.dst.len() - self.pos
    }

    pub fn current(&self) -> &[u8] {
        &self.dst[self.pos..]
    }

    /// Reserve the whole record before writing to avoid partial writes.
    pub fn write_canonical<T: CanonicalRecord>(&mut self, value: &T) -> Result<(), String> {
        if self.remaining() < T::SIZE {
            return Err(WRITER_OVERFLOW.to_string());
        }
        value.encode_into(self)
    }
}

/// Bounds-checked little-endian reader over caller-provided memory.
pub struct StateReader<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> StateReader<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        Self { src, pos: 0 }
    }

    fn reserve(&mut self, size: usize) -> Result<&[u8], String> {
        if self.remaining() < size {
            return Err(READER_UNDERFLOW.to_string());
        }
        let at = self.pos;
        self.pos += size;
        Ok(&self.src[at..at + size])
    }

    pub fn read_bytes(&mut self, dst: &mut [u8]) -> Result<(), String> {
        if dst.is_empty() {
            return Ok(());
        }
        dst.copy_from_slice(self.reserve(dst.len())?);
        Ok(())
    }

    pub fn read_u8(&mut self) -> Result<u8, String> {
        Ok(self.reserve(1)?[0])
    }

    pub fn read_u16(&mut self) -> Result<u16, String> {
        let bytes = self.reserve(2)?;
        Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
    }

    pub fn read_i16(&mut self) -> Result<i16, String> {
        let bytes = self.reserve(2)?;
        Ok(i16::from_le_bytes([bytes[0], bytes[1]]))
    }

    pub fn read_u32(&mut self) -> Result<u32, String> {
        let bytes = self.reserve(4)?;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    pub fn read_i32(&mut self) -> Result<i32, String> {
        let bytes = self.reserve(4)?;
        Ok(i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    pub fn advance(&mut self, size: usize) -> Result<(), String> {
        self.reserve(size).map(|_| ())
    }

    pub fn skip(&mut self, size: usize) -> Result<(), String> {
        self.advance(size)
    }

    pub fn remaining(&self) -> usize {
        self.src.len() - self.pos
    }

    pub fn current(&self) -> &[u8] {
        &self.src[self.pos..]
    }

    /// Read one packed canonical record, then validate it like `validate_state_value`.
    pub fn read_canonical<T: CanonicalRecord>(&mut self) -> Result<T, String> {
        if self.remaining() < T::SIZE {
            return Err(READER_UNDERFLOW.to_string());
        }
        let value = T::decode_from(self)?;
        value.validate()?;
        Ok(value)
    }
}

/// One `#pragma pack(1)` snapshot record.
pub trait CanonicalRecord: Copy {
    /// Packed byte size, pinned by a compile-time assertion at the definition.
    const SIZE: usize;

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String>;

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String>;

    /// `validate_state_value` for this record; the default accepts every bit pattern.
    fn validate(&self) -> Result<(), String> {
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Canonical records
// ---------------------------------------------------------------------------

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalSceneSprite {
    pub x: i32,
    pub y: i32,
    pub scale_x: u16,
    pub scale_y: u16,
    pub tile: u32,
    pub palette: u8,
    pub flip_x: u8,
    pub flip_y: u8,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalSceneLayer {
    pub priority: u8,
    pub blend_mode: u8,
    pub clip_enabled: u8,
    pub clip_inverted: u8,
    pub clip_inverse: u8,
    pub enabled: u8,
    pub blend_select: u8,
    pub mosaic: u8,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalScenePlayfield {
    pub layer: CanonicalSceneLayer,
    pub source_x: i32,
    pub source_y: i32,
    pub x_step: i32,
    pub y_step: i32,
    pub y_fraction: u8,
    pub palette_add: u16,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalSceneClip {
    pub left: i16,
    pub right: i16,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalSceneRow {
    pub playfields: [CanonicalScenePlayfield; 4],
    pub sprites: [CanonicalSceneLayer; 4],
    pub text: CanonicalSceneLayer,
    pub clips: [CanonicalSceneClip; 4],
    pub blend: [u8; 4],
    pub background: u16,
    pub text_x: i16,
    pub text_y: i16,
    pub mosaic_period: u8,
    pub bitmap: u8,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalLinePivot {
    pub mix_value: u16,
    pub prio: u8,
    pub blend_mode: u8,
    pub clip_enable: u8,
    pub clip_inv: u8,
    pub clip_inv_mode: u8,
    pub blend_select_v: u8,
    pub x_sample_enable: u8,
    pub pivot_control: u8,
    pub pivot_enable: u16,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalLineSprite {
    pub mix_value: u16,
    pub prio: u8,
    pub blend_mode: u8,
    pub clip_enable: u8,
    pub clip_inv: u8,
    pub clip_inv_mode: u8,
    pub blend_select_v: u8,
    pub x_sample_enable: u8,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalLinePlayfield {
    pub mix_value: u16,
    pub prio: u8,
    pub blend_mode: u8,
    pub clip_enable: u8,
    pub clip_inv: u8,
    pub clip_inv_mode: u8,
    pub x_sample_enable: u8,
    pub colscroll: u16,
    pub x_scale: i32,
    pub y_scale: i32,
    pub pal_add: u16,
    pub rowscroll: i32,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalLineParams {
    pub clip: [CanonicalSceneClip; 4],
    pub blend: [u8; 4],
    pub x_sample: u8,
    pub bg_palette: u16,
    pub pivot: CanonicalLinePivot,
    pub sp: [CanonicalLineSprite; 4],
    pub pf: [CanonicalLinePlayfield; 4],
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CanonicalGameVideoHeader {
    pub rendered: u8,
    pub sprite_staging_count: u32,
    pub sprite_submitted_count: u32,
    pub sprite_current_count: u32,
    pub sprite_current_flipped: u8,
    pub sprite_current_pen_mask: u8,
    pub sprite_current_trails: u8,
    pub sprite_reg_flipped: u8,
    pub sprite_reg_pen_mask: u8,
    pub sprite_reg_trails: u8,
    pub sprite_reg_bank: u8,
}

// Packed snapshot layout pins: a size change means the on-disk/netplay snapshot format changed.
const _: () = assert!(size_of::<CanonicalSceneSprite>() == 19);
const _: () = assert!(size_of::<CanonicalSceneLayer>() == 8);
const _: () = assert!(size_of::<CanonicalScenePlayfield>() == 27);
const _: () = assert!(size_of::<CanonicalSceneClip>() == 4);
const _: () = assert!(size_of::<CanonicalSceneRow>() == 176);
const _: () = assert!(size_of::<CanonicalLinePivot>() == 12);
const _: () = assert!(size_of::<CanonicalLineSprite>() == 9);
const _: () = assert!(size_of::<CanonicalLinePlayfield>() == 24);
const _: () = assert!(size_of::<CanonicalLineParams>() == 167);
const _: () = assert!(size_of::<CanonicalGameVideoHeader>() == 20);

fn write_layer(writer: &mut StateWriter<'_>, layer: &CanonicalSceneLayer) -> Result<(), String> {
    // Reading a packed field by value copies it; taking a reference would be unaligned.
    let layer = *layer;
    writer.write_u8(layer.priority)?;
    writer.write_u8(layer.blend_mode)?;
    writer.write_u8(layer.clip_enabled)?;
    writer.write_u8(layer.clip_inverted)?;
    writer.write_u8(layer.clip_inverse)?;
    writer.write_u8(layer.enabled)?;
    writer.write_u8(layer.blend_select)?;
    writer.write_u8(layer.mosaic)
}

fn read_layer(reader: &mut StateReader<'_>) -> Result<CanonicalSceneLayer, String> {
    Ok(CanonicalSceneLayer {
        priority: reader.read_u8()?,
        blend_mode: reader.read_u8()?,
        clip_enabled: reader.read_u8()?,
        clip_inverted: reader.read_u8()?,
        clip_inverse: reader.read_u8()?,
        enabled: reader.read_u8()?,
        blend_select: reader.read_u8()?,
        mosaic: reader.read_u8()?,
    })
}

fn write_clip(writer: &mut StateWriter<'_>, clip: &CanonicalSceneClip) -> Result<(), String> {
    let clip = *clip;
    writer.write_i16(clip.left)?;
    writer.write_i16(clip.right)
}

fn read_clip(reader: &mut StateReader<'_>) -> Result<CanonicalSceneClip, String> {
    Ok(CanonicalSceneClip {
        left: reader.read_i16()?,
        right: reader.read_i16()?,
    })
}

impl CanonicalRecord for CanonicalSceneSprite {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let value = *self;
        writer.write_i32(value.x)?;
        writer.write_i32(value.y)?;
        writer.write_u16(value.scale_x)?;
        writer.write_u16(value.scale_y)?;
        writer.write_u32(value.tile)?;
        writer.write_u8(value.palette)?;
        writer.write_u8(value.flip_x)?;
        writer.write_u8(value.flip_y)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        Ok(Self {
            x: reader.read_i32()?,
            y: reader.read_i32()?,
            scale_x: reader.read_u16()?,
            scale_y: reader.read_u16()?,
            tile: reader.read_u32()?,
            palette: reader.read_u8()?,
            flip_x: reader.read_u8()?,
            flip_y: reader.read_u8()?,
        })
    }

    fn validate(&self) -> Result<(), String> {
        let value = *self;
        if value.flip_x > 1 {
            return Err(canonical_error("CanonicalSceneSprite::flip_x"));
        }
        if value.flip_y > 1 {
            return Err(canonical_error("CanonicalSceneSprite::flip_y"));
        }
        Ok(())
    }
}

impl CanonicalRecord for CanonicalSceneLayer {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        write_layer(writer, self)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        read_layer(reader)
    }
}

impl CanonicalRecord for CanonicalScenePlayfield {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let value = *self;
        write_layer(writer, &value.layer)?;
        writer.write_i32(value.source_x)?;
        writer.write_i32(value.source_y)?;
        writer.write_i32(value.x_step)?;
        writer.write_i32(value.y_step)?;
        writer.write_u8(value.y_fraction)?;
        writer.write_u16(value.palette_add)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        Ok(Self {
            layer: read_layer(reader)?,
            source_x: reader.read_i32()?,
            source_y: reader.read_i32()?,
            x_step: reader.read_i32()?,
            y_step: reader.read_i32()?,
            y_fraction: reader.read_u8()?,
            palette_add: reader.read_u16()?,
        })
    }
}

impl CanonicalRecord for CanonicalSceneClip {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        write_clip(writer, self)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        read_clip(reader)
    }
}

fn validate_scene_layer(layer: &CanonicalSceneLayer) -> Result<(), String> {
    let layer = *layer;
    if layer.priority > 15
        || layer.blend_mode > 3
        || layer.clip_enabled > 15
        || layer.clip_inverted > 15
        || layer.clip_inverse > 1
        || layer.enabled > 1
        || layer.blend_select > 1
        || layer.mosaic > 1
    {
        return Err(canonical_error("CanonicalSceneLayer"));
    }
    Ok(())
}

impl CanonicalRecord for CanonicalSceneRow {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let playfields = self.playfields;
        for playfield in &playfields {
            write_layer(writer, &playfield.layer)?;
            let playfield = *playfield;
            writer.write_i32(playfield.source_x)?;
            writer.write_i32(playfield.source_y)?;
            writer.write_i32(playfield.x_step)?;
            writer.write_i32(playfield.y_step)?;
            writer.write_u8(playfield.y_fraction)?;
            writer.write_u16(playfield.palette_add)?;
        }
        let sprites = self.sprites;
        for sprite in &sprites {
            write_layer(writer, sprite)?;
        }
        let text = self.text;
        write_layer(writer, &text)?;
        let clips = self.clips;
        for clip in &clips {
            write_clip(writer, clip)?;
        }
        let value = *self;
        writer.write_bytes(&value.blend)?;
        writer.write_u16(value.background)?;
        writer.write_i16(value.text_x)?;
        writer.write_i16(value.text_y)?;
        writer.write_u8(value.mosaic_period)?;
        writer.write_u8(value.bitmap)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        let mut row = Self::default();
        for playfield in &mut row.playfields {
            playfield.layer = read_layer(reader)?;
            playfield.source_x = reader.read_i32()?;
            playfield.source_y = reader.read_i32()?;
            playfield.x_step = reader.read_i32()?;
            playfield.y_step = reader.read_i32()?;
            playfield.y_fraction = reader.read_u8()?;
            playfield.palette_add = reader.read_u16()?;
        }
        for sprite in &mut row.sprites {
            *sprite = read_layer(reader)?;
        }
        row.text = read_layer(reader)?;
        for clip in &mut row.clips {
            *clip = read_clip(reader)?;
        }
        reader.read_bytes(&mut row.blend)?;
        row.background = reader.read_u16()?;
        row.text_x = reader.read_i16()?;
        row.text_y = reader.read_i16()?;
        row.mosaic_period = reader.read_u8()?;
        row.bitmap = reader.read_u8()?;
        Ok(row)
    }

    fn validate(&self) -> Result<(), String> {
        let playfields = self.playfields;
        for playfield in &playfields {
            validate_scene_layer(&playfield.layer)?;
        }
        let sprites = self.sprites;
        for sprite in &sprites {
            validate_scene_layer(sprite)?;
        }
        let text = self.text;
        validate_scene_layer(&text)?;
        if self.bitmap > 1 {
            return Err(canonical_error("CanonicalSceneRow::bitmap"));
        }
        Ok(())
    }
}

impl CanonicalRecord for CanonicalLinePivot {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let value = *self;
        writer.write_u16(value.mix_value)?;
        writer.write_u8(value.prio)?;
        writer.write_u8(value.blend_mode)?;
        writer.write_u8(value.clip_enable)?;
        writer.write_u8(value.clip_inv)?;
        writer.write_u8(value.clip_inv_mode)?;
        writer.write_u8(value.blend_select_v)?;
        writer.write_u8(value.x_sample_enable)?;
        writer.write_u8(value.pivot_control)?;
        writer.write_u16(value.pivot_enable)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        Ok(Self {
            mix_value: reader.read_u16()?,
            prio: reader.read_u8()?,
            blend_mode: reader.read_u8()?,
            clip_enable: reader.read_u8()?,
            clip_inv: reader.read_u8()?,
            clip_inv_mode: reader.read_u8()?,
            blend_select_v: reader.read_u8()?,
            x_sample_enable: reader.read_u8()?,
            pivot_control: reader.read_u8()?,
            pivot_enable: reader.read_u16()?,
        })
    }
}

impl CanonicalRecord for CanonicalLineSprite {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let value = *self;
        writer.write_u16(value.mix_value)?;
        writer.write_u8(value.prio)?;
        writer.write_u8(value.blend_mode)?;
        writer.write_u8(value.clip_enable)?;
        writer.write_u8(value.clip_inv)?;
        writer.write_u8(value.clip_inv_mode)?;
        writer.write_u8(value.blend_select_v)?;
        writer.write_u8(value.x_sample_enable)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        Ok(Self {
            mix_value: reader.read_u16()?,
            prio: reader.read_u8()?,
            blend_mode: reader.read_u8()?,
            clip_enable: reader.read_u8()?,
            clip_inv: reader.read_u8()?,
            clip_inv_mode: reader.read_u8()?,
            blend_select_v: reader.read_u8()?,
            x_sample_enable: reader.read_u8()?,
        })
    }
}

impl CanonicalRecord for CanonicalLinePlayfield {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let value = *self;
        writer.write_u16(value.mix_value)?;
        writer.write_u8(value.prio)?;
        writer.write_u8(value.blend_mode)?;
        writer.write_u8(value.clip_enable)?;
        writer.write_u8(value.clip_inv)?;
        writer.write_u8(value.clip_inv_mode)?;
        writer.write_u8(value.x_sample_enable)?;
        writer.write_u16(value.colscroll)?;
        writer.write_i32(value.x_scale)?;
        writer.write_i32(value.y_scale)?;
        writer.write_u16(value.pal_add)?;
        writer.write_i32(value.rowscroll)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        Ok(Self {
            mix_value: reader.read_u16()?,
            prio: reader.read_u8()?,
            blend_mode: reader.read_u8()?,
            clip_enable: reader.read_u8()?,
            clip_inv: reader.read_u8()?,
            clip_inv_mode: reader.read_u8()?,
            x_sample_enable: reader.read_u8()?,
            colscroll: reader.read_u16()?,
            x_scale: reader.read_i32()?,
            y_scale: reader.read_i32()?,
            pal_add: reader.read_u16()?,
            rowscroll: reader.read_i32()?,
        })
    }
}

impl CanonicalRecord for CanonicalLineParams {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let clips = self.clip;
        for clip in &clips {
            write_clip(writer, clip)?;
        }
        let value = *self;
        writer.write_bytes(&value.blend)?;
        writer.write_u8(value.x_sample)?;
        writer.write_u16(value.bg_palette)?;
        write_pivot(writer, &value.pivot)?;
        let sprites = self.sp;
        for sprite in &sprites {
            write_line_sprite(writer, sprite)?;
        }
        let playfields = self.pf;
        for playfield in &playfields {
            write_line_playfield(writer, playfield)?;
        }
        Ok(())
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        let mut params = Self::default();
        for clip in &mut params.clip {
            *clip = read_clip(reader)?;
        }
        reader.read_bytes(&mut params.blend)?;
        params.x_sample = reader.read_u8()?;
        params.bg_palette = reader.read_u16()?;
        params.pivot = read_pivot(reader)?;
        for sprite in &mut params.sp {
            *sprite = read_line_sprite(reader)?;
        }
        for playfield in &mut params.pf {
            *playfield = read_line_playfield(reader)?;
        }
        Ok(params)
    }
}

fn write_pivot(writer: &mut StateWriter<'_>, pivot: &CanonicalLinePivot) -> Result<(), String> {
    let pivot = *pivot;
    writer.write_u16(pivot.mix_value)?;
    writer.write_u8(pivot.prio)?;
    writer.write_u8(pivot.blend_mode)?;
    writer.write_u8(pivot.clip_enable)?;
    writer.write_u8(pivot.clip_inv)?;
    writer.write_u8(pivot.clip_inv_mode)?;
    writer.write_u8(pivot.blend_select_v)?;
    writer.write_u8(pivot.x_sample_enable)?;
    writer.write_u8(pivot.pivot_control)?;
    writer.write_u16(pivot.pivot_enable)
}

fn read_pivot(reader: &mut StateReader<'_>) -> Result<CanonicalLinePivot, String> {
    Ok(CanonicalLinePivot {
        mix_value: reader.read_u16()?,
        prio: reader.read_u8()?,
        blend_mode: reader.read_u8()?,
        clip_enable: reader.read_u8()?,
        clip_inv: reader.read_u8()?,
        clip_inv_mode: reader.read_u8()?,
        blend_select_v: reader.read_u8()?,
        x_sample_enable: reader.read_u8()?,
        pivot_control: reader.read_u8()?,
        pivot_enable: reader.read_u16()?,
    })
}

fn write_line_sprite(
    writer: &mut StateWriter<'_>,
    sprite: &CanonicalLineSprite,
) -> Result<(), String> {
    let sprite = *sprite;
    writer.write_u16(sprite.mix_value)?;
    writer.write_u8(sprite.prio)?;
    writer.write_u8(sprite.blend_mode)?;
    writer.write_u8(sprite.clip_enable)?;
    writer.write_u8(sprite.clip_inv)?;
    writer.write_u8(sprite.clip_inv_mode)?;
    writer.write_u8(sprite.blend_select_v)?;
    writer.write_u8(sprite.x_sample_enable)
}

fn read_line_sprite(reader: &mut StateReader<'_>) -> Result<CanonicalLineSprite, String> {
    Ok(CanonicalLineSprite {
        mix_value: reader.read_u16()?,
        prio: reader.read_u8()?,
        blend_mode: reader.read_u8()?,
        clip_enable: reader.read_u8()?,
        clip_inv: reader.read_u8()?,
        clip_inv_mode: reader.read_u8()?,
        blend_select_v: reader.read_u8()?,
        x_sample_enable: reader.read_u8()?,
    })
}

fn write_line_playfield(
    writer: &mut StateWriter<'_>,
    playfield: &CanonicalLinePlayfield,
) -> Result<(), String> {
    let playfield = *playfield;
    writer.write_u16(playfield.mix_value)?;
    writer.write_u8(playfield.prio)?;
    writer.write_u8(playfield.blend_mode)?;
    writer.write_u8(playfield.clip_enable)?;
    writer.write_u8(playfield.clip_inv)?;
    writer.write_u8(playfield.clip_inv_mode)?;
    writer.write_u8(playfield.x_sample_enable)?;
    writer.write_u16(playfield.colscroll)?;
    writer.write_i32(playfield.x_scale)?;
    writer.write_i32(playfield.y_scale)?;
    writer.write_u16(playfield.pal_add)?;
    writer.write_i32(playfield.rowscroll)
}

fn read_line_playfield(reader: &mut StateReader<'_>) -> Result<CanonicalLinePlayfield, String> {
    Ok(CanonicalLinePlayfield {
        mix_value: reader.read_u16()?,
        prio: reader.read_u8()?,
        blend_mode: reader.read_u8()?,
        clip_enable: reader.read_u8()?,
        clip_inv: reader.read_u8()?,
        clip_inv_mode: reader.read_u8()?,
        x_sample_enable: reader.read_u8()?,
        colscroll: reader.read_u16()?,
        x_scale: reader.read_i32()?,
        y_scale: reader.read_i32()?,
        pal_add: reader.read_u16()?,
        rowscroll: reader.read_i32()?,
    })
}

impl CanonicalRecord for CanonicalGameVideoHeader {
    const SIZE: usize = size_of::<Self>();

    fn encode_into(&self, writer: &mut StateWriter<'_>) -> Result<(), String> {
        let value = *self;
        writer.write_u8(value.rendered)?;
        writer.write_u32(value.sprite_staging_count)?;
        writer.write_u32(value.sprite_submitted_count)?;
        writer.write_u32(value.sprite_current_count)?;
        writer.write_u8(value.sprite_current_flipped)?;
        writer.write_u8(value.sprite_current_pen_mask)?;
        writer.write_u8(value.sprite_current_trails)?;
        writer.write_u8(value.sprite_reg_flipped)?;
        writer.write_u8(value.sprite_reg_pen_mask)?;
        writer.write_u8(value.sprite_reg_trails)?;
        writer.write_u8(value.sprite_reg_bank)
    }

    fn decode_from(reader: &mut StateReader<'_>) -> Result<Self, String> {
        Ok(Self {
            rendered: reader.read_u8()?,
            sprite_staging_count: reader.read_u32()?,
            sprite_submitted_count: reader.read_u32()?,
            sprite_current_count: reader.read_u32()?,
            sprite_current_flipped: reader.read_u8()?,
            sprite_current_pen_mask: reader.read_u8()?,
            sprite_current_trails: reader.read_u8()?,
            sprite_reg_flipped: reader.read_u8()?,
            sprite_reg_pen_mask: reader.read_u8()?,
            sprite_reg_trails: reader.read_u8()?,
            sprite_reg_bank: reader.read_u8()?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn record_bytes<T: CanonicalRecord>(value: &T) -> Vec<u8> {
        let mut bytes = vec![0u8; T::SIZE];
        let mut writer = StateWriter::new(&mut bytes);
        writer.write_canonical(value).unwrap();
        assert_eq!(writer.remaining(), 0);
        bytes
    }

    fn read_record<T: CanonicalRecord>(bytes: &[u8]) -> Result<T, String> {
        let mut reader = StateReader::new(bytes);
        let value = reader.read_canonical::<T>()?;
        assert_eq!(reader.remaining(), 0);
        Ok(value)
    }

    #[test]
    fn packed_sizes_match_the_cpp_snapshot_layout() {
        assert_eq!(CanonicalSceneSprite::SIZE, 19);
        assert_eq!(CanonicalSceneLayer::SIZE, 8);
        assert_eq!(CanonicalScenePlayfield::SIZE, 27);
        assert_eq!(CanonicalSceneClip::SIZE, 4);
        assert_eq!(CanonicalSceneRow::SIZE, 176);
        assert_eq!(CanonicalLinePivot::SIZE, 12);
        assert_eq!(CanonicalLineSprite::SIZE, 9);
        assert_eq!(CanonicalLinePlayfield::SIZE, 24);
        assert_eq!(CanonicalLineParams::SIZE, 167);
        assert_eq!(CanonicalGameVideoHeader::SIZE, 20);
    }

    #[test]
    fn primitive_writes_are_little_endian_and_sequential() {
        let mut bytes = [0u8; 14];
        let mut writer = StateWriter::new(&mut bytes);
        writer.write_u8(0x12).unwrap();
        writer.write_u16(0x3456).unwrap();
        writer.write_i16(-2).unwrap();
        writer.write_u32(0x789a_bcde).unwrap();
        writer.write_i32(-3).unwrap();
        writer.write_u8(0x01).unwrap();
        assert_eq!(writer.remaining(), 0);
        assert_eq!(
            bytes,
            [
                0x12, 0x56, 0x34, 0xfe, 0xff, 0xde, 0xbc, 0x9a, 0x78, 0xfd, 0xff, 0xff, 0xff, 0x01
            ]
        );
    }

    #[test]
    fn overflow_is_reported_before_any_partial_write() {
        let mut bytes = [0u8; 4];
        let mut writer = StateWriter::new(&mut bytes);
        assert_eq!(writer.write_u32(0x1122_3344), Ok(()));
        assert_eq!(writer.write_u8(1), Err(WRITER_OVERFLOW.to_string()));
        assert_eq!(
            writer.write_bytes(&[1, 2]),
            Err(WRITER_OVERFLOW.to_string())
        );
        assert_eq!(writer.advance(1), Err(WRITER_OVERFLOW.to_string()));
        assert_eq!(writer.remaining(), 0);

        // A canonical record reserves its full size first, so a short buffer stays untouched.
        let mut bytes = [0u8; 6];
        let mut writer = StateWriter::new(&mut bytes);
        assert_eq!(
            writer.write_canonical(&CanonicalSceneSprite::default()),
            Err(WRITER_OVERFLOW.to_string())
        );
        assert_eq!(bytes, [0u8; 6]);
    }

    #[test]
    fn underflow_is_reported() {
        let bytes = [0u8; 3];
        let mut reader = StateReader::new(&bytes);
        assert_eq!(reader.read_u32(), Err(READER_UNDERFLOW.to_string()));
        assert_eq!(reader.read_u16(), Ok(0));
        assert_eq!(reader.read_u8(), Ok(0));
        assert_eq!(reader.read_u8(), Err(READER_UNDERFLOW.to_string()));
        assert_eq!(reader.skip(1), Err(READER_UNDERFLOW.to_string()));
        assert_eq!(
            reader.read_canonical::<CanonicalSceneClip>(),
            Err(READER_UNDERFLOW.to_string())
        );
    }

    #[test]
    fn zero_sized_writes_are_no_ops() {
        let mut bytes = [7u8; 1];
        let mut writer = StateWriter::new(&mut bytes);
        assert_eq!(writer.write_bytes(&[]), Ok(()));
        assert_eq!(writer.remaining(), 1);
        let mut reader = StateReader::new(&[]);
        assert_eq!(reader.read_bytes(&mut []), Ok(()));
        assert_eq!(reader.remaining(), 0);
    }

    #[test]
    fn scene_sprite_round_trips() {
        let value = CanonicalSceneSprite {
            x: -300,
            y: 0x1234_5678,
            scale_x: 0x0102,
            scale_y: 0x0304,
            tile: 0xdead_beef,
            palette: 0x7f,
            flip_x: 1,
            flip_y: 0,
        };
        let bytes = record_bytes(&value);
        assert_eq!(bytes.len(), 19);
        let decoded = read_record::<CanonicalSceneSprite>(&bytes).unwrap();
        let (x, y, tile) = (decoded.x, decoded.y, decoded.tile);
        assert_eq!((x, y, tile), (-300, 0x1234_5678, 0xdead_beef));
        assert_eq!(record_bytes(&decoded), bytes);
    }

    #[test]
    fn scene_sprite_rejects_non_canonical_flips() {
        let mut bits = record_bytes(&CanonicalSceneSprite::default());
        bits[17] = 2;
        assert_eq!(
            read_record::<CanonicalSceneSprite>(&bits).unwrap_err(),
            canonical_error("CanonicalSceneSprite::flip_x")
        );
        let mut bits = record_bytes(&CanonicalSceneSprite::default());
        bits[18] = 0xff;
        assert_eq!(
            read_record::<CanonicalSceneSprite>(&bits).unwrap_err(),
            canonical_error("CanonicalSceneSprite::flip_y")
        );
    }

    #[test]
    fn scene_row_round_trips_and_validates_layers() {
        let mut value = CanonicalSceneRow::default();
        value.playfields[3].layer.priority = 15;
        value.playfields[3].layer.clip_enabled = 9;
        value.playfields[3].layer.clip_inverted = 0x0b;
        value.playfields[3].layer.clip_inverse = 1;
        value.playfields[3].layer.enabled = 1;
        value.playfields[3].layer.blend_select = 1;
        value.playfields[3].layer.mosaic = 1;
        value.playfields[3].source_x = -77;
        value.playfields[3].source_y = 511;
        value.playfields[3].x_step = 256;
        value.playfields[3].y_step = 512;
        value.playfields[3].y_fraction = 0x42;
        value.playfields[3].palette_add = 0x1234;
        value.sprites[1].priority = 3;
        value.sprites[1].blend_mode = 2;
        value.text.priority = 4;
        value.clips[2].left = -12;
        value.clips[2].right = 300;
        value.blend = [1, 2, 3, 8];
        value.background = 0xbeef;
        value.text_x = -3;
        value.text_y = 511;
        value.mosaic_period = 15;
        value.bitmap = 1;

        let bytes = record_bytes(&value);
        assert_eq!(bytes.len(), 176);
        let decoded = read_record::<CanonicalSceneRow>(&bytes).unwrap();
        assert_eq!(record_bytes(&decoded), bytes);

        let mut bits = record_bytes(&value);
        bits[176 - 1] = 2; // bitmap
        assert_eq!(
            read_record::<CanonicalSceneRow>(&bits).unwrap_err(),
            canonical_error("CanonicalSceneRow::bitmap")
        );

        let mut bits = record_bytes(&value);
        bits[0] = 16; // first playfield layer priority
        assert_eq!(
            read_record::<CanonicalSceneRow>(&bits).unwrap_err(),
            canonical_error("CanonicalSceneLayer")
        );
    }

    #[test]
    fn line_params_round_trip() {
        let mut value = CanonicalLineParams::default();
        for (i, clip) in value.clip.iter_mut().enumerate() {
            clip.left = i as i16 - 1;
            clip.right = 300 + i as i16;
        }
        value.blend = [8, 7, 6, 5];
        value.x_sample = 13;
        value.bg_palette = 0x3344;
        value.pivot.mix_value = 0x0f0f;
        value.pivot.prio = 15;
        value.pivot.blend_mode = 3;
        value.pivot.clip_enable = 0xa;
        value.pivot.clip_inv = 0x5;
        value.pivot.clip_inv_mode = 1;
        value.pivot.blend_select_v = 1;
        value.pivot.x_sample_enable = 1;
        value.pivot.pivot_control = 0xa0;
        value.pivot.pivot_enable = 0xbeef;
        for (i, sprite) in value.sp.iter_mut().enumerate() {
            sprite.mix_value = 0x3000 + i as u16;
            sprite.prio = i as u8;
            sprite.blend_mode = 2;
            sprite.clip_enable = 1;
            sprite.clip_inv = 2;
            sprite.clip_inv_mode = 1;
            sprite.blend_select_v = 1;
            sprite.x_sample_enable = 1;
        }
        for (i, playfield) in value.pf.iter_mut().enumerate() {
            playfield.mix_value = 0x2000 + i as u16;
            playfield.prio = i as u8 + 1;
            playfield.blend_mode = 1;
            playfield.clip_enable = 3;
            playfield.clip_inv = 4;
            playfield.clip_inv_mode = 1;
            playfield.x_sample_enable = 1;
            playfield.colscroll = 0x1ff;
            playfield.x_scale = 200;
            playfield.y_scale = 512;
            playfield.pal_add = 64;
            playfield.rowscroll = -100;
        }
        let bytes = record_bytes(&value);
        assert_eq!(bytes.len(), 167);
        let decoded = read_record::<CanonicalLineParams>(&bytes).unwrap();
        assert_eq!(record_bytes(&decoded), bytes);
    }

    #[test]
    fn game_video_header_round_trip() {
        let value = CanonicalGameVideoHeader {
            rendered: 1,
            sprite_staging_count: 5,
            sprite_submitted_count: 0xdead_beef,
            sprite_current_count: 1024,
            sprite_current_flipped: 1,
            sprite_current_pen_mask: 0x3f,
            sprite_current_trails: 1,
            sprite_reg_flipped: 1,
            sprite_reg_pen_mask: 0x0f,
            sprite_reg_trails: 1,
            sprite_reg_bank: 1,
        };
        let bytes = record_bytes(&value);
        assert_eq!(bytes.len(), 20);
        let decoded = read_record::<CanonicalGameVideoHeader>(&bytes).unwrap();
        assert_eq!(record_bytes(&decoded), bytes);
    }
}
