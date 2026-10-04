-- Capture the real sound ROM's device writes for independent audio parity.
-- F3_AUDIO_TRACE is a binary output path. Run the clean baseline with -wavwrite
-- and a fresh -nvram_directory. No ROM data is included in this script.
if _G.f3rt_audio_trace_started then return end
_G.f3rt_audio_trace_started = true
local path = assert(os.getenv("F3_AUDIO_TRACE"), "Set F3_AUDIO_TRACE output path")
local file = assert(io.open(path, "wb"))
file:write("F3AUD1\0\0")
local space = assert(manager.machine.devices[":taito_en:audiocpu"]).spaces["program"]
local function install()
    _G.f3rt_audio_tap = space:install_write_tap(0x200000, 0x340003, "f3rt_audio", function(address, data, mask)
        -- Emulated timestamp in 16 MHz main-clock ticks, then a 16-bit bus write.
        file:write(string.pack("<I8I4I2I2", math.floor(emu.time() * 16000000 + 0.5), address, data, mask))
    end)
end
install()
_G.f3rt_audio_reset_subscription = emu.add_machine_reset_notifier(install)
_G.f3rt_audio_stop_subscription = emu.add_machine_stop_notifier(function()
    file:write(string.pack("<I8I4I2I2", math.floor(emu.time() * 16000000 + 0.5), 0xffffffff, 0, 0))
    file:close()
    print("[AudioTrace] Wrote " .. path)
end)
