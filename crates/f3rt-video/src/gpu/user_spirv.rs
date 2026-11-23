//! Postprocess shader loading and ABI validation.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// `MAX_USER_SHADER_BYTES`.
pub const MAX_USER_SHADER_BYTES: usize = 1024 * 1024;

/// SPIR-V opcodes and ABI constants used by the validator.
pub mod spirv {
    pub const MAGIC: u32 = 0x0723_0203;
    #[cfg(test)]
    pub const VERSION_1_0: u32 = 0x0001_0000;
    pub const ENTRY_MAIN_WORD: u32 = 0x6e69_616d;

    #[cfg(test)]
    pub const OP_MEMORY_MODEL: u32 = 14;
    pub const OP_ENTRY_POINT: u32 = 15;
    #[cfg(test)]
    pub const OP_EXECUTION_MODE: u32 = 16;
    #[cfg(test)]
    pub const OP_CAPABILITY: u32 = 17;
    #[cfg(test)]
    pub const OP_TYPE_VOID: u32 = 19;
    pub const OP_TYPE_FLOAT: u32 = 22;
    pub const OP_TYPE_VECTOR: u32 = 23;
    pub const OP_TYPE_IMAGE: u32 = 25;
    pub const OP_TYPE_SAMPLED_IMAGE: u32 = 27;
    pub const OP_TYPE_STRUCT: u32 = 30;
    pub const OP_TYPE_POINTER: u32 = 32;
    #[cfg(test)]
    pub const OP_TYPE_FUNCTION: u32 = 33;
    #[cfg(test)]
    pub const OP_FUNCTION: u32 = 54;
    #[cfg(test)]
    pub const OP_FUNCTION_END: u32 = 56;
    pub const OP_VARIABLE: u32 = 59;
    pub const OP_DECORATE: u32 = 71;
    pub const OP_MEMBER_DECORATE: u32 = 72;
    #[cfg(test)]
    pub const OP_LABEL: u32 = 248;
    #[cfg(test)]
    pub const OP_RETURN: u32 = 253;

    #[cfg(test)]
    pub const CAPABILITY_SHADER: u32 = 1;
    #[cfg(test)]
    pub const ADDRESSING_LOGICAL: u32 = 0;
    #[cfg(test)]
    pub const MEMORY_MODEL_GLSL450: u32 = 1;
    pub const EXEC_MODEL_FRAGMENT: u32 = 4;
    #[cfg(test)]
    pub const EXEC_MODE_ORIGIN_UPPER_LEFT: u32 = 7;

    pub const DECORATION_BLOCK: u32 = 2;
    pub const DECORATION_BUILTIN: u32 = 11;
    #[cfg(test)]
    pub const DECORATION_LOCATION: u32 = 30;
    pub const DECORATION_BINDING: u32 = 33;
    pub const DECORATION_DESCRIPTOR_SET: u32 = 34;
    pub const DECORATION_OFFSET: u32 = 35;

    pub const STORAGE_UNIFORM_CONSTANT: u32 = 0;
    pub const STORAGE_INPUT: u32 = 1;
    pub const STORAGE_UNIFORM: u32 = 2;
    pub const STORAGE_OUTPUT: u32 = 3;
    pub const STORAGE_PRIVATE: u32 = 6;
    pub const STORAGE_FUNCTION: u32 = 7;

    pub const DIM_2D: u32 = 1;
    pub const IMAGE_SAMPLED_YES: u32 = 1;
}

use spirv::*;

/// A user postprocess shader read from disk. `code` is 4-byte aligned storage: SPIR-V words for
/// Vulkan, or the MSL text plus one trailing zero byte for Metal. `size` is the payload in bytes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LoadedUserShader {
    pub code: Vec<u32>,
    pub size: usize,
}

#[derive(Clone, Copy, Debug, Default)]
struct Id {
    opcode: u32,
    first_operand: u32,
    second_operand: u32,
    word_count: u32,
    set: u32,
    binding: u32,
    builtin: bool,
    block: bool,
    zero_offset: bool,
}

impl Id {
    fn new() -> Self {
        Self {
            set: u32::MAX,
            binding: u32::MAX,
            ..Self::default()
        }
    }
}

const INVALID: &str = "User SPIR-V: invalid module or postprocess ABI";

fn invalid<T>() -> Result<T, String> {
    Err(INVALID.to_owned())
}

fn id(ids: &[Id], n: u32) -> Result<&Id, String> {
    if n == 0 || n as usize >= ids.len() {
        return invalid();
    }
    Ok(&ids[n as usize])
}

fn id_mut(ids: &mut [Id], n: u32) -> Result<&mut Id, String> {
    if n == 0 || n as usize >= ids.len() {
        return invalid();
    }
    Ok(&mut ids[n as usize])
}

/// Validates the fixed descriptor/interface ABI before user SPIR-V reaches the driver.
pub fn validate_user_spirv(words: &[u32]) -> Result<(), String> {
    if words.len() < 5
        || words[0] != MAGIC
        || words[4] != 0
        || words[3] == 0
        || words[3] > (MAX_USER_SHADER_BYTES / 4) as u32
    {
        return invalid();
    }
    let mut ids = vec![Id::new(); words[3] as usize];
    let mut entry = false;
    let mut pos = 5usize;
    while pos < words.len() {
        let p = &words[pos..];
        let count = p[0] >> 16;
        let op = p[0] & 0xffff;
        if count == 0 || count as usize > words.len() - pos {
            return invalid();
        }
        let need = |n: u32| -> Result<(), String> { if count < n { invalid() } else { Ok(()) } };
        if op == OP_ENTRY_POINT {
            need(5)?;
            if entry || p[1] != EXEC_MODEL_FRAGMENT || p[3] != ENTRY_MAIN_WORD || p[4] != 0 {
                return invalid();
            }
            id(&ids, p[2])?;
            entry = true;
        } else if op == OP_DECORATE {
            need(3)?;
            id(&ids, p[1])?;
            if p[2] == DECORATION_BINDING || p[2] == DECORATION_DESCRIPTOR_SET {
                need(4)?;
                if p[2] == DECORATION_BINDING {
                    id_mut(&mut ids, p[1])?.binding = p[3];
                } else {
                    id_mut(&mut ids, p[1])?.set = p[3];
                }
            } else if p[2] == DECORATION_BUILTIN {
                id_mut(&mut ids, p[1])?.builtin = true;
            } else if p[2] == DECORATION_BLOCK {
                id_mut(&mut ids, p[1])?.block = true;
            }
        } else if op == OP_MEMBER_DECORATE {
            need(4)?;
            if p[3] == DECORATION_OFFSET {
                need(5)?;
                if p[2] == 0 && p[4] == 0 {
                    id_mut(&mut ids, p[1])?.zero_offset = true;
                }
            }
        } else if op == OP_TYPE_FLOAT
            || op == OP_TYPE_VECTOR
            || op == OP_TYPE_IMAGE
            || op == OP_TYPE_SAMPLED_IMAGE
            || op == OP_TYPE_STRUCT
            || op == OP_TYPE_POINTER
            || op == OP_VARIABLE
        {
            let unary_type =
                op == OP_TYPE_FLOAT || op == OP_TYPE_SAMPLED_IMAGE || op == OP_TYPE_STRUCT;
            need(if unary_type { 3 } else { 4 })?;
            let result = if op == OP_VARIABLE { p[2] } else { p[1] };
            let v = id_mut(&mut ids, result)?;
            // A second result ID definition would
            // silently overwrite the first and could pass the ABI check.
            if v.opcode != 0 {
                return invalid();
            }
            v.opcode = op;
            v.first_operand = if op == OP_VARIABLE { p[1] } else { p[2] };
            v.second_operand = if count > 3 { p[3] } else { 0 };
            v.word_count = count;
            if op == OP_TYPE_IMAGE {
                need(9)?;
                let non_arrayed_2d_sampled = p[3] == DIM_2D
                    && p[4] == 0
                    && p[5] == 0
                    && p[6] == 0
                    && p[7] == IMAGE_SAMPLED_YES;
                if !non_arrayed_2d_sampled {
                    return invalid();
                }
            }
        }
        pos += count as usize;
    }
    if !entry {
        return invalid();
    }
    let mut samplers = 0;
    let mut uniforms = 0;
    for i in 0..ids.len() {
        if ids[i].opcode != OP_VARIABLE {
            continue;
        }
        let pointer = *id(&ids, ids[i].first_operand)?;
        if pointer.opcode != OP_TYPE_POINTER || pointer.first_operand != ids[i].second_operand {
            return invalid();
        }
        let ty = *id(&ids, pointer.second_operand)?;
        let storage = ids[i].second_operand;
        if storage == STORAGE_UNIFORM_CONSTANT {
            if ids[i].set != 2 || ids[i].binding != 0 || ty.opcode != OP_TYPE_SAMPLED_IMAGE {
                return invalid();
            }
            let image = *id(&ids, ty.first_operand)?;
            let sampled_type = *id(&ids, image.first_operand)?;
            if image.opcode != OP_TYPE_IMAGE
                || sampled_type.opcode != OP_TYPE_FLOAT
                || sampled_type.first_operand != 32
            {
                return invalid();
            }
            samplers += 1;
        } else if storage == STORAGE_UNIFORM {
            if ids[i].set != 3
                || ids[i].binding != 0
                || ty.opcode != OP_TYPE_STRUCT
                || ty.word_count != 3
                || !ty.block
                || !ty.zero_offset
            {
                return invalid();
            }
            let vector = *id(&ids, ty.first_operand)?;
            let component = *id(&ids, vector.first_operand)?;
            if vector.opcode != OP_TYPE_VECTOR
                || vector.second_operand != 4
                || component.opcode != OP_TYPE_FLOAT
                || component.first_operand != 32
            {
                return invalid();
            }
            uniforms += 1;
        } else if storage == STORAGE_INPUT {
            if !ids[i].builtin {
                return invalid();
            }
        } else if storage != STORAGE_OUTPUT
            && storage != STORAGE_PRIVATE
            && storage != STORAGE_FUNCTION
        {
            return invalid();
        }
    }
    if samplers != 1 || uniforms != 1 {
        return invalid();
    }
    Ok(())
}

/// Reads and checks a user shader file: extension (`.metal` / `.spv`), size (1 byte..1 MiB),
/// no embedded NUL for Metal, 4-byte multiple plus `validate_user_spirv` for Vulkan.
pub fn load_user_shader(path: &str, metal: bool) -> Result<LoadedUserShader, String> {
    let expected = if metal { "metal" } else { "spv" };
    if Path::new(path).extension().and_then(|e| e.to_str()) != Some(expected) {
        return Err(if metal {
            "Metal user shader requires a .metal file"
        } else {
            "Vulkan user shader requires a .spv file"
        }
        .to_owned());
    }
    let open_error = |_| format!("Cannot open user shader: {path}");
    let mut file = File::open(path).map_err(open_error)?;
    let end = file.seek(SeekFrom::End(0)).map_err(open_error)?;
    if end == 0 || end > MAX_USER_SHADER_BYTES as u64 {
        return Err("User shader must be 1 byte to 1 MiB".to_owned());
    }
    let mut shader = LoadedUserShader {
        code: vec![0; (end as usize + 4) / 4],
        size: end as usize,
    };
    // SAFETY: the initialized word storage covers `size` bytes; every bit pattern is a valid u32.
    let bytes = unsafe {
        std::slice::from_raw_parts_mut(shader.code.as_mut_ptr().cast::<u8>(), shader.size)
    };
    let read_error = |_| format!("Cannot read user shader: {path}");
    file.seek(SeekFrom::Start(0)).map_err(read_error)?;
    file.read_exact(bytes).map_err(read_error)?;
    if metal {
        if bytes.contains(&0) {
            return Err("User Metal shader contains an embedded NUL".to_owned());
        }
        return Ok(shader);
    }
    if !shader.size.is_multiple_of(4) {
        return Err("User SPIR-V size must be a multiple of four".to_owned());
    }
    shader.code.truncate(shader.size / 4);
    validate_user_spirv(&shader.code)?;
    Ok(shader)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[derive(Clone, Copy)]
    struct SpirvShape {
        execution_model: u32,
        entry_name: u32,
        sampler_set: u32,
        sampler_binding: u32,
        block_set: u32,
        block_binding: u32,
        member_offset: u32,
        block_decoration: bool,
        second_member: bool,
        sampled_dimension: u32,
    }

    impl Default for SpirvShape {
        fn default() -> Self {
            Self {
                execution_model: EXEC_MODEL_FRAGMENT,
                entry_name: ENTRY_MAIN_WORD,
                sampler_set: 2,
                sampler_binding: 0,
                block_set: 3,
                block_binding: 0,
                member_offset: 0,
                block_decoration: true,
                second_member: false,
                sampled_dimension: DIM_2D,
            }
        }
    }

    #[allow(clippy::too_many_lines)]
    fn spirv_module(shape: SpirvShape) -> Vec<u32> {
        const MAIN: u32 = 1;
        const VOID: u32 = 2;
        const FN_TYPE: u32 = 3;
        const FLOAT: u32 = 4;
        const VEC4: u32 = 5;
        const IMAGE: u32 = 6;
        const SAMPLED_IMAGE: u32 = 7;
        const SAMPLER_PTR: u32 = 8;
        const SAMPLER: u32 = 9;
        const BLOCK: u32 = 10;
        const BLOCK_PTR: u32 = 11;
        const BLOCK_VAR: u32 = 12;
        const OUT_PTR: u32 = 13;
        const OUT_VAR: u32 = 14;
        const LABEL: u32 = 15;
        const BOUND: u32 = 16;

        let mut w = vec![MAGIC, VERSION_1_0, 0, BOUND, 0];
        let mut op = |code: u32, operands: &[u32]| {
            w.push((operands.len() as u32 + 1) << 16 | code);
            w.extend_from_slice(operands);
        };
        op(OP_CAPABILITY, &[CAPABILITY_SHADER]);
        op(OP_MEMORY_MODEL, &[ADDRESSING_LOGICAL, MEMORY_MODEL_GLSL450]);
        op(
            OP_ENTRY_POINT,
            &[shape.execution_model, MAIN, shape.entry_name, 0, OUT_VAR],
        );
        op(OP_EXECUTION_MODE, &[MAIN, EXEC_MODE_ORIGIN_UPPER_LEFT]);
        op(
            OP_DECORATE,
            &[SAMPLER, DECORATION_DESCRIPTOR_SET, shape.sampler_set],
        );
        op(
            OP_DECORATE,
            &[SAMPLER, DECORATION_BINDING, shape.sampler_binding],
        );
        op(
            OP_DECORATE,
            &[BLOCK_VAR, DECORATION_DESCRIPTOR_SET, shape.block_set],
        );
        op(
            OP_DECORATE,
            &[BLOCK_VAR, DECORATION_BINDING, shape.block_binding],
        );
        op(OP_DECORATE, &[OUT_VAR, DECORATION_LOCATION, 0]);
        if shape.block_decoration {
            op(OP_DECORATE, &[BLOCK, DECORATION_BLOCK]);
        }
        op(
            OP_MEMBER_DECORATE,
            &[BLOCK, 0, DECORATION_OFFSET, shape.member_offset],
        );
        op(OP_TYPE_VOID, &[VOID]);
        op(OP_TYPE_FUNCTION, &[FN_TYPE, VOID]);
        op(OP_TYPE_FLOAT, &[FLOAT, 32]);
        op(OP_TYPE_VECTOR, &[VEC4, FLOAT, 4]);
        op(
            OP_TYPE_IMAGE,
            &[
                IMAGE,
                FLOAT,
                shape.sampled_dimension,
                0,
                0,
                0,
                IMAGE_SAMPLED_YES,
                0,
            ],
        );
        op(OP_TYPE_SAMPLED_IMAGE, &[SAMPLED_IMAGE, IMAGE]);
        op(
            OP_TYPE_POINTER,
            &[SAMPLER_PTR, STORAGE_UNIFORM_CONSTANT, SAMPLED_IMAGE],
        );
        op(
            OP_VARIABLE,
            &[SAMPLER_PTR, SAMPLER, STORAGE_UNIFORM_CONSTANT],
        );
        if shape.second_member {
            op(OP_TYPE_STRUCT, &[BLOCK, VEC4, VEC4]);
        } else {
            op(OP_TYPE_STRUCT, &[BLOCK, VEC4]);
        }
        op(OP_TYPE_POINTER, &[BLOCK_PTR, STORAGE_UNIFORM, BLOCK]);
        op(OP_VARIABLE, &[BLOCK_PTR, BLOCK_VAR, STORAGE_UNIFORM]);
        op(OP_TYPE_POINTER, &[OUT_PTR, STORAGE_OUTPUT, VEC4]);
        op(OP_VARIABLE, &[OUT_PTR, OUT_VAR, STORAGE_OUTPUT]);
        op(OP_FUNCTION, &[VOID, MAIN, 0, FN_TYPE]);
        op(OP_LABEL, &[LABEL]);
        op(OP_RETURN, &[]);
        op(OP_FUNCTION_END, &[]);
        w
    }

    #[test]
    fn accepts_postprocess_abi() {
        validate_user_spirv(&spirv_module(SpirvShape::default())).unwrap();
    }

    #[test]
    fn rejects_broken_abi() {
        let rejects = |words: &[u32]| {
            assert!(validate_user_spirv(words).is_err());
        };
        rejects(&[]);
        let mut bad_magic = spirv_module(SpirvShape::default());
        bad_magic[0] ^= 1;
        rejects(&bad_magic);
        let mut bad_schema = spirv_module(SpirvShape::default());
        bad_schema[4] = 1;
        rejects(&bad_schema);
        let mut zero_bound = spirv_module(SpirvShape::default());
        zero_bound[3] = 0;
        rejects(&zero_bound);
        let mut oversized = spirv_module(SpirvShape::default());
        oversized[3] = (MAX_USER_SHADER_BYTES / 4) as u32 + 1;
        rejects(&oversized);
        let mut truncated = spirv_module(SpirvShape::default());
        truncated.pop();
        truncated.pop();
        truncated.push(0x00ff_0000 | 56);
        rejects(&truncated);

        rejects(&spirv_module(SpirvShape {
            execution_model: 0,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            entry_name: 0x6e69_616e,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            sampler_set: 1,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            sampler_binding: 1,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            block_set: 2,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            block_binding: 1,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            member_offset: 16,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            block_decoration: false,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            second_member: true,
            ..SpirvShape::default()
        }));
        rejects(&spirv_module(SpirvShape {
            sampled_dimension: 2,
            ..SpirvShape::default()
        }));
    }

    proptest::proptest! {
        #![proptest_config(proptest::test_runner::Config::with_cases(48))]

        #[test]
        fn rejects_invalid_headers_duplicate_ids_and_malformed_instructions(
            damage in 0u8..6,
            result_id in 4u32..=12,
            bad_word in 1u32..=u16::MAX as u32,
        ) {
            use proptest::prelude::*;
            let mut words = spirv_module(SpirvShape::default());
            prop_assert_eq!(validate_user_spirv(&words), Ok(()));
            match damage {
                0 => words[0] ^= bad_word,
                1 => words[4] = bad_word,
                2 => words[3] = 0,
                3 => words.extend([(3 << 16) | OP_TYPE_FLOAT, result_id, 32]),
                4 => words.push(OP_FUNCTION_END),
                _ => words.push(((bad_word + 1).min(u16::MAX as u32) << 16) | OP_FUNCTION_END),
            }
            prop_assert_eq!(validate_user_spirv(&words).unwrap_err(), INVALID);
        }
    }

    #[test]
    fn rejects_duplicate_result_ids() {
        let mut words = spirv_module(SpirvShape::default());
        // Redefine FLOAT (result id 4) after OP_FUNCTION_END.
        words.push(3 << 16 | OP_TYPE_FLOAT);
        words.push(4);
        words.push(32);
        assert_eq!(
            validate_user_spirv(&words).unwrap_err(),
            "User SPIR-V: invalid module or postprocess ABI"
        );
    }

    fn temp_path(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("f3rt-user-spirv-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join(name)
    }

    struct ShaderFile {
        path: PathBuf,
    }

    impl ShaderFile {
        fn new(name: &str, bytes: &[u8]) -> Self {
            let path = temp_path(name);
            std::fs::write(&path, bytes).unwrap();
            Self { path }
        }
    }

    impl Drop for ShaderFile {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.path);
        }
    }

    #[test]
    fn loads_and_checks_shader_files() {
        let words = spirv_module(SpirvShape::default());
        let good = ShaderFile::new(
            "good.spv",
            &words
                .iter()
                .flat_map(|w| w.to_le_bytes())
                .collect::<Vec<_>>(),
        );
        let loaded = load_user_shader(good.path.to_str().unwrap(), false).unwrap();
        assert_eq!(loaded.size, words.len() * 4);
        assert_eq!(loaded.code, words);

        assert_eq!(
            load_user_shader(good.path.to_str().unwrap(), true).unwrap_err(),
            "Metal user shader requires a .metal file"
        );
        let odd_bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let unaligned = ShaderFile::new("odd.spv", &odd_bytes[..odd_bytes.len() - 1]);
        assert_eq!(
            load_user_shader(unaligned.path.to_str().unwrap(), false).unwrap_err(),
            "User SPIR-V size must be a multiple of four"
        );
        let empty = ShaderFile::new("empty.spv", &[]);
        assert_eq!(
            load_user_shader(empty.path.to_str().unwrap(), false).unwrap_err(),
            "User shader must be 1 byte to 1 MiB"
        );
        assert!(
            load_user_shader(temp_path("missing.spv").to_str().unwrap(), false)
                .unwrap_err()
                .starts_with("Cannot open user shader:")
        );
        let mut broken = words.clone();
        broken[0] ^= 1;
        let broken_bytes: Vec<u8> = broken.iter().flat_map(|w| w.to_le_bytes()).collect();
        let bad_abi = ShaderFile::new("bad.spv", &broken_bytes);
        assert_eq!(
            load_user_shader(bad_abi.path.to_str().unwrap(), false).unwrap_err(),
            "User SPIR-V: invalid module or postprocess ABI"
        );

        let msl = b"fragment float4 main0() { return 0; }";
        let metal = ShaderFile::new("post.metal", msl);
        let shader = load_user_shader(metal.path.to_str().unwrap(), true).unwrap();
        assert_eq!(shader.size, msl.len());
        let mut text = Vec::new();
        for word in &shader.code {
            text.extend_from_slice(&word.to_le_bytes());
        }
        assert_eq!(&text[..msl.len()], msl, "MSL text keeps its trailing zero");
        assert_eq!(text[msl.len()], 0);

        let nul = ShaderFile::new("nul.metal", b"a\0b");
        assert_eq!(
            load_user_shader(nul.path.to_str().unwrap(), true).unwrap_err(),
            "User Metal shader contains an embedded NUL"
        );
    }
}
