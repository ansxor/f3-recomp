-- Run with MAME -autoboot_script tools/mame/capture.lua -nothrottle.
-- F3_CAPTURE_PREFIX is an existing output-directory plus basename.
-- F3_CAPTURE_FRAMES selects comma-separated frame indices after script start.
local prefix = assert(os.getenv("F3_CAPTURE_PREFIX"), "set F3_CAPTURE_PREFIX")
local wanted, last = {}, 0
for value in (os.getenv("F3_CAPTURE_FRAMES") or "1,60,300,600,1200"):gmatch("%d+") do
    local n = tonumber(value)
    wanted[n] = true
    last = math.max(last, n)
end
assert(last > 0, "at least one capture frame is required")
local cpu = assert(manager.machine.devices[":maincpu"], "maincpu missing")
local space = assert(cpu.spaces["program"], "program space missing")
local screen = assert(manager.machine.screens[":screen"], "screen missing")
local log = assert(io.open(prefix .. "-frames.tsv", "w"))
log:write("capture_frame\tmame_frame\tpc\tsr\td0\td1\td2\td3\td4\td5\td6\td7\ta0\ta1\ta2\ta3\ta4\ta5\ta6\ta7\n")
local function register(name)
    return assert(cpu.state[name], "missing register " .. name).value
end
local function dump(path, first, final)
    local file = assert(io.open(path, "wb"))
    -- 8-bit reads make on-disk byte order independent of host/core bus width.
    file:write(space:read_range(first, final, 8))
    file:close()
end
local frame = 0
local subscription
subscription = emu.add_machine_frame_notifier(function()
    frame = frame + 1
    if wanted[frame] then
        local base = string.format("%s-%06d", prefix, frame)
        screen:snapshot(base .. ".png")
        dump(base .. "-ram.bin", 0x400000, 0x41ffff)
        dump(base .. "-palette.bin", 0x440000, 0x447fff)
        dump(base .. "-video.bin", 0x600000, 0x63ffff)
        log:write(string.format("%d\t%d\t%08x\t%04x", frame, screen:frame_number(), register("PC"), register("SR")))
        for i = 0, 7 do log:write(string.format("\t%08x", register("D" .. i))) end
        for i = 0, 7 do log:write(string.format("\t%08x", register("A" .. i))) end
        log:write("\n")
        log:flush()
    end
    if frame == last then
        log:close()
        subscription:unsubscribe()
        manager.machine:exit()
    end
end)
