-- tools/mame/capture.lua
-- MAME trace capture script for Land Maker (landmakrj) attract mode.
-- Run with: mame landmakrj -autoboot_script tools/mame/capture.lua -autoboot_delay 0 -wavwrite captures/landmakrj_attract/attract.wav
--
-- Directory protocol specification:
--   captures/landmakrj_attract/
--     metadata.json
--     frame_0300/
--       metadata.json
--       control.bin          (0x20 bytes, BE: 16 words of m_control_0 and m_control_1)
--       palette.bin          (0x8000 bytes, BE: 8192 x 32-bit color entries)
--       graphics.bin         (0x40000 bytes, BE: spriteram, pf_ram, textram, charram, lineram, pivot)
--       mainram.bin          (0x20000 bytes, BE: 0x400000..0x41ffff)
--       spriteram_active.bin (0x10000 bytes, BE: sprite lag buffered spriteram actually rendered)
--       reference.argb       (320*232*4 bytes: raw screen pixels from MAME)
--       reference.bmp        (320x232 32-bit standard BMP image for instant inspection)

local outdir = os.getenv("F3_CAPTURE_DIR") or "captures/landmakrj_attract"
local start_frame = tonumber(os.getenv("F3_CAPTURE_START")) or 300
local capture_count = tonumber(os.getenv("F3_CAPTURE_COUNT")) or 10
local capture_step = tonumber(os.getenv("F3_CAPTURE_STEP")) or 1
local auto_exit = (os.getenv("F3_CAPTURE_EXIT") ~= "0")

-- Autoboot can run again after a game-requested soft reset; retain the original
-- tap and frame numbering rather than registering a second capture callback.
if _G.f3rt_capture_started then return end
_G.f3rt_capture_started = true

print("=============================================================")
print("  F3rt MAME Capture Script initialized")
print("  Target Game:      landmakrj (Land Maker)")
print("  Output Directory: " .. outdir)
print("  Start Frame:      " .. start_frame)
print("  Capture Count:    " .. capture_count .. " frames (step " .. capture_step .. ")")
print("  Auto Exit:        " .. tostring(auto_exit))
print("=============================================================")

local function ensure_dir(path)
    os.execute("mkdir -p '" .. path .. "'")
end

local function write_binary_file(path, data)
    local f, err = io.open(path, "wb")
    if not f then
        print("[MameCapture] ERROR opening " .. path .. ": " .. tostring(err))
        return false
    end
    f:write(data)
    f:close()
    return true
end

local function write_text_file(path, text)
    local f, err = io.open(path, "w")
    if not f then
        print("[MameCapture] ERROR opening " .. path .. ": " .. tostring(err))
        return false
    end
    f:write(text)
    f:close()
    return true
end

local function le16(v)
    local b0 = v % 256
    local b1 = math.floor(v / 256) % 256
    return string.char(b0, b1)
end

local function le32(v)
    local b0 = v % 256
    local b1 = math.floor(v / 256) % 256
    local b2 = math.floor(v / 65536) % 256
    local b3 = math.floor(v / 16777216) % 256
    return string.char(b0, b1, b2, b3)
end

-- Writes a standard 32-bit bottom-up BMP image from BGRA pixel buffer.
local function write_bmp_32(path, w, h, bgra_data)
    local row_bytes = w * 4
    local file_size = 54 + row_bytes * h
    local file_hdr = "BM" .. le32(file_size) .. le16(0) .. le16(0) .. le32(54)
    local info_hdr = le32(40) .. le32(w) .. le32(h) .. le16(1) .. le16(32) .. le32(0) .. le32(row_bytes * h) .. le32(2835) .. le32(2835) .. le32(0) .. le32(0)

    local f, err = io.open(path, "wb")
    if not f then
        print("[MameCapture] ERROR writing BMP " .. path .. ": " .. tostring(err))
        return false
    end
    f:write(file_hdr)
    f:write(info_hdr)

    -- Write rows from bottom to top (h-1 down to 0)
    for y = h - 1, 0, -1 do
        local offset = y * row_bytes + 1
        local row = string.sub(bgra_data, offset, offset + row_bytes - 1)
        f:write(row)
    end
    f:close()
    return true
end

ensure_dir(outdir)

-- Locate Main CPU and memory space
local cpu = manager.machine.devices[":maincpu"]
if not cpu then
    error("[MameCapture] Fatal: :maincpu device not found!")
end

local space = cpu.spaces["program"]
if not space then
    error("[MameCapture] Fatal: :maincpu 'program' address space not found!")
end

local screen = manager.machine.screens[":screen"]
if not screen then
    error("[MameCapture] Fatal: :screen device not found!")
end

-- Write-only control registers table (0x660000..0x66001f, 32 bytes)
-- 0x660000..0x66000f: m_control_0[8] (pf scroll X/Y)
-- 0x660010..0x66001f: m_control_1[8] (pivot/text scroll X/Y)
local ctrl_regs = {}
for i = 0, 31 do
    ctrl_regs[i] = 0
end

local function extract_byte(val, shift)
    return math.floor(val / (2 ^ shift)) % 256
end

local function has_mask(mask, shift)
    return (math.floor(mask / (2 ^ shift)) % 256) ~= 0
end

-- Reset remaps the program space and drops taps. Reinstall before CPU execution.
local ctrl_tap
local function install_control_tap()
    ctrl_tap = space:install_write_tap(0x660000, 0x66001f, "f3_ctrl_tap", function(offset, data, mem_mask)
        local rel = offset - 0x660000
        if rel >= 0 and rel <= 28 then
            if has_mask(mem_mask, 24) then ctrl_regs[rel + 0] = extract_byte(data, 24) end
            if has_mask(mem_mask, 16) then ctrl_regs[rel + 1] = extract_byte(data, 16) end
            if has_mask(mem_mask, 8)  then ctrl_regs[rel + 2] = extract_byte(data, 8)  end
            if has_mask(mem_mask, 0)  then ctrl_regs[rel + 3] = extract_byte(data, 0)  end
        end
    end)
    _G.f3rt_ctrl_tap = ctrl_tap
end
install_control_tap()
local reset_subscription = emu.add_machine_reset_notifier(install_control_tap)
_G.f3rt_reset_subscription = reset_subscription

print("[MameCapture] Installed write tap on control registers 0x660000..0x66001f")

-- State variables for frame tracking
local frame_counter = 0
local captured_count = 0
local captured_frames_meta = {}

-- Buffer history for sprite lag (sprite_lag == 1)
-- At frame N:
--   scanline_draw() renders sprites from spriteram of frame N-1.
--   get_sprite_info() then updates sprite framebuffer from current spriteram for frame N+1.
local prev_spriteram = nil

local function on_frame_done()
    frame_counter = frame_counter + 1

    -- Read current spriteram (0x600000..0x60ffff, 0x10000 bytes)
    local current_spriteram = space:read_range(0x600000, 0x60ffff, 8)
    local active_spriteram = prev_spriteram or current_spriteram
    prev_spriteram = current_spriteram

    -- Check if this frame should be captured
    if frame_counter < start_frame then
        return
    end

    if (frame_counter - start_frame) % capture_step ~= 0 then
        return
    end

    if captured_count >= capture_count then
        return
    end

    captured_count = captured_count + 1
    local frame_tag = string.format("frame_%04d", frame_counter)
    local frame_dir = outdir .. "/" .. frame_tag
    ensure_dir(frame_dir)

    -- 1. Control registers (0x20 bytes BE)
    local ctrl_bytes = {}
    local ctrl_hex = {}
    for i = 0, 31 do
        local b = ctrl_regs[i] or 0
        ctrl_bytes[i + 1] = string.char(b)
        ctrl_hex[i + 1] = string.format("%02x", b)
    end
    local ctrl_bin = table.concat(ctrl_bytes)
    write_binary_file(frame_dir .. "/control.bin", ctrl_bin)

    -- 2. Palette RAM (0x8000 bytes BE, 0x440000..0x447fff)
    local palette_bin = space:read_range(0x440000, 0x447fff, 8)
    write_binary_file(frame_dir .. "/palette.bin", palette_bin)

    -- 3. Graphics RAM (0x40000 bytes BE, 0x600000..0x63ffff)
    local graphics_bin = space:read_range(0x600000, 0x63ffff, 8)
    write_binary_file(frame_dir .. "/graphics.bin", graphics_bin)

    -- 4. Main RAM (0x20000 bytes BE, 0x400000..0x41ffff)
    local mainram_bin = space:read_range(0x400000, 0x41ffff, 8)
    write_binary_file(frame_dir .. "/mainram.bin", mainram_bin)
    write_binary_file(frame_dir .. "/shared.bin", space:read_range(0xc00000, 0xc007ff, 8))
    local state = { string.format('{"pc":%d,"sr":%d,"d":[', cpu.state["PC"].value, cpu.state["SR"].value) }
    for i = 0, 7 do state[#state + 1] = (i > 0 and "," or "") .. tostring(cpu.state["D" .. i].value) end
    state[#state + 1] = '],"a":['
    for i = 0, 7 do state[#state + 1] = (i > 0 and "," or "") .. tostring(cpu.state["A" .. i].value) end
    state[#state + 1] = ']}\n'
    write_text_file(frame_dir .. "/cpu.json", table.concat(state))
    local sound_cpu = manager.machine.devices[":taito_en:audiocpu"]
    if sound_cpu then
        write_binary_file(frame_dir .. "/soundram.bin", sound_cpu.spaces["program"]:read_range(0, 0xffff, 8))
        write_text_file(frame_dir .. "/sound_cpu.json", string.format('{"pc":%d,"sr":%d}\n',
            sound_cpu.state["PC"].value, sound_cpu.state["SR"].value))
    end

    -- 5. Active Spriteram (0x10000 bytes BE, from frame N-1)
    write_binary_file(frame_dir .. "/spriteram_active.bin", active_spriteram)

    -- 6. Screen pixels (visible resolution 320x232)
    local pixels, w, h = screen:pixels()
    write_binary_file(frame_dir .. "/reference.argb", pixels)
    write_bmp_32(frame_dir .. "/reference.bmp", w, h, pixels)

    -- 7. Frame metadata JSON
    local frame_meta_json = string.format([[{
  "frame_index": %d,
  "width": %d,
  "height": %d,
  "pixel_format": "ARGB32_LE_BGRA",
  "files": {
    "control": "control.bin",
    "palette": "palette.bin",
    "graphics": "graphics.bin",
    "mainram": "mainram.bin",
    "spriteram_active": "spriteram_active.bin",
    "reference_argb": "reference.argb",
    "reference_bmp": "reference.bmp"
  },
  "control_regs_hex": "%s"
}]], frame_counter, w, h, table.concat(ctrl_hex, ""))
    write_text_file(frame_dir .. "/metadata.json", frame_meta_json)

    table.insert(captured_frames_meta, string.format([[    { "frame_index": %d, "dir": "%s" }]], frame_counter, frame_tag))
    print(string.format("[MameCapture] Captured %s (%d/%d): %dx%d pixels, palette 0x%x, gfx 0x%x",
        frame_tag, captured_count, capture_count, w, h, #palette_bin, #graphics_bin))

    -- Check completion
    if captured_count >= capture_count then
        -- 8. Write overall metadata.json
        local visarea = { min_x = 46, max_x = 365, min_y = 24, max_y = 255 }
        local global_meta_json = string.format([[{
  "format_version": 1,
  "game": "landmakrj",
  "description": "MAME Land Maker attract mode trace capture",
  "visible_width": %d,
  "visible_height": %d,
  "visarea": {
    "min_x": %d,
    "max_x": %d,
    "min_y": %d,
    "max_y": %d
  },
  "pixel_format": "ARGB32_LE_BGRA",
  "sprite_lag": 1,
  "extend": 1,
  "palette_size": 32768,
  "graphics_size": 262144,
  "control_size": 32,
  "mainram_size": 131072,
  "spriteram_size": 65536,
  "total_frames_captured": %d,
  "frames": [
%s
  ]
}]], w, h, visarea.min_x, visarea.max_x, visarea.min_y, visarea.max_y, captured_count, table.concat(captured_frames_meta, ",\n"))
        write_text_file(outdir .. "/metadata.json", global_meta_json)
        print("[MameCapture] Capture completed successfully! Wrote metadata.json")

        if auto_exit then
            print("[MameCapture] Exiting MAME...")
            manager.machine:exit()
        end
    end
end

emu.register_frame_done(on_frame_done)
print("[MameCapture] Frame capture callback registered. Running emulation...")
