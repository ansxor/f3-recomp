# DUART, volume chip and mixer gain

The DUART supplies driver timing and DSP control. The MB87078 sets channel attenuation. This page explains both devices and the board mixer.

Sources: [mc68681.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/third_party/audio/mc68681.cpp), [mb87078.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/third_party/audio/mb87078.cpp), and [audio.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/audio.cpp).

## The MC68681 DUART

DUART means dual universal asynchronous receiver/transmitter. The Motorola MC68681 has two serial ports, a counter/timer, input pins and output pins. The F3 sound board does not use the serial ports for communication. It uses the chip for three jobs:

1. The counter/timer makes a periodic interrupt. This is the tick of the sound driver.
2. Output pin OP6 halts or releases the ES5510 DSP.
3. The interrupt vector register (IVR) gives the interrupt vector number.

The class is `f3rt::MC68681` in `runtime/third_party/audio/mc68681.*`. `Audio` calls `advance(duart_cycles)` with 4 MHz ticks (main ticks divided by 4).

### Registers

The sound CPU reaches the DUART at `0x280000` - `0x28001f`. Only odd addresses carry data. The register number is `(address >> 1) & 0x0f`. For example, register `0xc` (IVR) is at `0x280019`.

The same register number means different registers for a read and a write.

| No. | Read | Write |
|---|---|---|
| `0x0` | `MR1A` / `MR2A` (a pointer toggles) | `MR1A` / `MR2A` |
| `0x1` | `SRA` status | `CSRA` clock select |
| `0x2` | not mapped (returns `0xff`) | `CRA` command |
| `0x3` | `RHRA` (always 0) | `THRA` transmit |
| `0x4` | `IPCR` input port change | `ACR` auxiliary control |
| `0x5` | `ISR` interrupt status | `IMR` interrupt mask |
| `0x6` | `CTU` counter high | `CTUR` preset high |
| `0x7` | `CTL` counter low | `CTLR` preset low |
| `0x8` - `0xb` | Same as `0x0` - `0x3`, channel B | Same, channel B |
| `0xc` | `IVR` | `IVR` |
| `0xd` | `IP` input pins (returns `0xbf`) | `OPCR` output configuration |
| `0xe` | Start counter command | Set output bits (`OPR`) |
| `0xf` | Stop counter command | Reset output bits |

A read of register `0xa` returns `0x61` (the test register). The code ignores all receive data: the receivers never get a byte.

### Interrupt

`irq_pending()` is true when `(ISR & IMR) != 0`. `Audio::irq_level()` returns 6 when this is true, and 0 when it is not. The ISR bits that the code defines are:

| Bit | Name |
|---|---|
| `0x01` | `INT_TXRDY_A` |
| `0x02` | `INT_RXRDY_A` |
| `0x08` | `INT_COUNTER_READY` |
| `0x10` | `INT_TXRDY_B` |
| `0x20` | `INT_RXRDY_B` |
| `0x80` | `INT_INPUT_PORT_CHANGE` |

When the sound CPU acknowledges level 6, `Audio::irq_ack(6)` returns the IVR. The reset value of IVR is `0x0f`. For other levels `irq_ack` returns `0x18 + level` (the 68000 autovector). The native driver turns a vector of 0 into 15.

The IRQ line is level-sensitive. It stays on while `ISR & IMR` is not zero. The handler must clear the cause. A read of register `0xf` (stop counter command) clears the `INT_COUNTER_READY` bit. The ROM IRQ handler reads `0x28001f` for this purpose, according to `docs/SOUND-DRIVER.md`.

### Counter and timer

The function `counter_divider()` gives the number of 4 MHz ticks for one count:

| `ACR` bit 6 | `ACR` bits 5-4 | Mode | Divider |
|---|---|---|---|
| 1 (timer) | 0 | IP2 (1 MHz) | 4 |
| 1 | 1 | IP2 / 16 | 64 |
| 1 | 2 | Crystal clock (4 MHz) | 1 |
| 1 | 3 | Crystal clock / 16 | 16 |
| 0 (counter) | 3 | | 16 |
| 0 | other | | 4 |

`start_counter()` loads `m_ct_remaining = max(preset, 1) * divider`. `advance()` subtracts the elapsed ticks.

- In **timer mode** (`ACR` bit 6 = 1) the counter reloads each time it expires and toggles `m_half_period`. The `INT_COUNTER_READY` bit sets when `m_half_period` goes back to 0. The interrupt period is therefore two counter periods.
- In **counter mode** the bit sets once. The counter then keeps counting from `0xffff`.

The driver of Land Maker programs the timer with preset `0x07d0` (2000) and `ACR = 0x60`, according to `docs/SOUND-DRIVER.md`. `0x60` is timer mode with the 4 MHz crystal clock and divider 1. One counter period is 2000 ticks of 4 MHz, which is 0.5 ms. The interrupt period is 1 ms, which is 16,000 main ticks. The document reports that the measured spacing of the IRQ reads has this mode value, with small jitter from instruction boundaries.

DUART reset preserves an armed timer deadline and its half-period phase. The next expiration uses the reset ACR, which selects counter mode.

Preset and source writes affect the next reload, not an already armed duration. Changing ACR bit 6 can start or stop the timer.

### Serial transmitters

The two transmitters exist because the driver reads and writes their status bits. Nothing is connected to the TX pins. The code models only these facts:

- `tx_write` models holding-register occupancy. It does not store transmitted byte data because the TX pins are disconnected.
- A full holding register rejects another byte.
- The frame length includes start, data, optional parity and stop bits. The model represents 1.5 stop bits as two.
- Clock selections 0–12 use baud tables. Selection 13 uses the counter-derived clock.
- Selection 14 uses the external clock divided by 16; selection 15 uses it directly.
- External clocks are 1 MHz for channel A and 500 kHz for channel B.
- `tx_bit` restores TXRDY during frame progress when the holding register is available. Frame completion sets TXEMT.
- `tx_interrupt` copies the TXRDY status to ISR.

The unit test `check_duart_tx` in `check.cpp` covers this behavior.

### Output pins and the ESP halt

The output port register `OPR` starts at 0. A write to register `0xe` sets bits. A write to register `0xf` clears bits. After each change, and after reset, the code calls the output callback with `OPR ^ 0xff`. The pins are active low.

`Audio::Impl` uses the callback for OP6 only:

```cpp
m_esp_halted = (output_pins & 0x40) != 0;
```

| DUART action | OP6 pin | `m_esp_halted` |
|---|---|---|
| Reset | high (OPR bit 6 = 0) | true |
| Write `0x40` to set register (`0x28001d`) | low | false (ESP runs) |
| Write `0x40` to reset register (`0x28001f`) | high | true (ESP halted) |

The addresses `0x28001d` and `0x28001f` come from `docs/SOUND-DRIVER.md`. They match register numbers `0xe` and `0xf`.

### Reset behavior

`reset()` clears `ACR`, `IMR`, `ISR`, `OPR` and the other registers, sets `IVR` to `0x0f`, and calls the output callback. A CPU-line reset does not reset the DUART. The check in `check.cpp` shows that a CPU-line reset keeps the DUART vector, and a watchdog reset restores it to `0x0f`.

Serial reset clears occupancy and enable state. It preserves idle clock phase and edge state.

Command-register actions include mode-pointer reset, receiver reset, transmitter reset, error reset, break-status clear, and transmitter enable or disable.

Receiver inputs are disconnected. Receive reads return zero; input-pin read returns the fixed value `0xbf`.

IPCR reads clear change flags and the input-port-change ISR bit. Mode-register reads advance the MR pointer to MR2.

Snapshots save timer deadline, phase, registers and both transmitter states. Loading restores them and updates output and IRQ callbacks.

## The MB87078 volume controller

The Fujitsu MB87078 is a 6-bit, 4-channel electronic volume controller. The class is `f3rt::MB87078`. The sound CPU reaches it at `0x340000` - `0x340003`. `Audio` uses even addresses only and swaps the two registers: address `0x340000` writes the **control** latch and address `0x340002` writes the **data** latch. The code does this with `offset ^ 1`.

Note the meaning. The two latches are not "left volume" and "right volume". The control latch picks a channel and flags. The data latch carries the attenuation. A write to the data latch commits the setting for the channel in the control latch.

```cpp
void MB87078::write(uint32_t offset, uint8_t data) {
    if (offset & 1) m_control = data & 0x1f;          // control latch
    else {
        m_data = data & 0x3f;                          // data latch
        m_channel_latch[m_control & 3] =
            ((uint16_t(m_control) << 4) & 0x1c0) | m_data;
        gain_recalc();
    }
}
```

`gain_recalc` picks the gain index for each of the four channels. The channel latch has 9 bits: the data (bits 0-5), `EN` (bit 6), `C0` (bit 7) and `C32` (bit 8).

| Condition | Gain index | Gain |
|---|---|---|
| `EN` is 0 | 65 | 0 (mute) |
| `C32` is 1 | 64 | -32 dB |
| `C0` is 1 | 0 | 0 dB |
| Otherwise | `~data & 0x3f` | `10^(-0.5 * index / 20)`, that is -0.5 dB per step |

Index 66 equals 0 dB and initializes each gain index before the constructor calls reset. Reset recalculation changes those indices to zero.

`Audio` attaches its callback later and initializes its own volume gains to 1.0. This agrees with the initial device gain.

When a gain index changes, the code calls the gain callback with `(channel, gain)`. `Audio` only uses channels 2 and 3: channel 2 is the left gain, and channel 3 is the right gain (`m_volume_gain[channel & 1]`). Channels 0 and 1 are not used in the mix.

`reset()` sets all channel latches to `0x7f`. This means enabled and 0 dB.

The header comment for `MB87078::write` labels offsets in reverse. The implementation uses offset 1 for control and offset 0 for data.

The board's `offset ^ 1` mapping therefore makes `0x340000` control and `0x340002` data.

Reads return the corresponding latch. Snapshots retain indices, channel latches and both host latches; static gain values are rebuilt at construction.

## The mixer gain

`Audio::Impl::update_gains` converts the physical gain of the chip into two multipliers for each side: `m_otis_gain` and `m_output_gain`. It runs when the volume chip calls back, when the model changes, and after a state load.

```cpp
const float route = float(int(physical * 100.0f + 0.5f)) / 32.0f;
m_otis_gain[ch]   = 0.18f * (MameRouting ? route : 1.0f);
m_output_gain[ch] = MameRouting ? route : physical;
```

The enum `Audio::GainModel` has two values.

| Model | OTIS gain | Output gain | Meaning |
|---|---|---|---|
| `MameRouting` (default) | `0.18 * route` | `route` | The volume applies twice: before the ESP input and after the pump. This matches the output that the MAME baseline gives. |
| `SingleStage` | `0.18` | `physical` | One analog stage after the ESP. The header says this is an unverified experiment. |

`route` is the physical gain as a percentage, rounded to an integer, divided by 32. At 0 dB the percentage is 100 and `route` is 3.125.

The final mix, from `generate_one_frame`:

```text
channels[i]  = clamp(otis_sum[i] / 524288, -1, 1) * m_otis_gain[i & 1]
ESP inputs   = int16(channels[2..7] * 32768)
out_left     = (ser_r(6) / 32768 + channels[0]) * 0.5 * m_output_gain[0]
out_right    = (ser_r(7) / 32768 + channels[1]) * 0.5 * m_output_gain[1]
```

Pair 0 skips the ESP and keeps float precision. The ESP path has 16-bit precision.

The runtime has no CLI option for `SingleStage`. Library code can select it through `set_gain_model`.

Existing `check_audio_mixer` covers side muting, a -6 dB case and the difference between gain models.

It also checks that CPU-line reset preserves attenuation while board reset restores it.

## Master volume in the game

The game sets the master volume. Main code writes four gain requests to the mailbox at `0xc007f8` - `0xc007fb` at about 13.23 seconds into the boot, according to `docs/SOUND-DRIVER.md`. This is why [the extraction tool](/developer/runtime/audio/extraction) runs 900 boot frames by default: earlier points keep the start-up attenuation.
