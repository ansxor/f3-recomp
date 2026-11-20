// Preset 12, decoded from the ROM and checked against the ES5510 interpreter.
// FX L/R -> mono -> high-pass -> rectifier and peak decay -> curve B -> return gain
// Mono + fractional tap -> high-pass -> 446-frame delay -> address modulation -> curve A
// Curve A -> five one-poles -> DAC and diffusion tank. The tank feeds the short return.
// The dry pair goes directly to the DAC, with the program's final output gain.
import("es5510.lib");

decay_gain = hslider("tank_decay", 0, -8388608, 8388607, 1);
allpass_gain = hslider("allpass", 0, -8388608, 8388607, 1);
allpass_complement = hslider("allpass_complement", 0, -8388608, 8388607, 1);
return_feedback = hslider("short_feedback", 0, -8388608, 8388607, 1);
rate = unit*(hslider("lfo_c_eps_hi", 0, -1, 1, 1e-9) + hslider("lfo_c_eps_lo", 0, -1e-6, 1e-6, 1e-15));
phase = hslider("lfo_c_ph_hi", 0, -7, 7, 1e-9) + hslider("lfo_c_ph_lo", 0, -1e-6, 1e-6, 1e-15);
limit = hslider("envelope_limit", 0, -8388608, 8388607, 1);
address_gain = hslider("address_gain", 0, -8388608, 8388607, 1);
tank_damp = hslider("tank_damp", 0, -8388608, 8388607, 1);
curve_gain = hslider("curve_gain", 0, -8388608, 8388607, 1);
filter_pole = hslider("filter_pole", 0, -8388608, 8388607, 1);
diffusion_pole = hslider("diffusion_pole", 0, -8388608, 8388607, 1);

// Signals are in Q23 counts. Doubles represent the full products and sums
// exactly, including the accumulator fraction used for the address calculations.
unit = 8388608.0;
full = 8388607.0;
clip(x) = min(full, max(-unit, x));
high(acc) = clip(floor(acc / unit));
low(acc) = acc - floor(acc / unit)*unit;
ram(x) = 256.0*floor(x / 256.0);
half(x) = floor(x*0.5);
multiply(x, gain) = high(x*gain);
interpolate(upper, lower, address) = upper*weight + lower*(full - weight)
with { weight = float(int(address) & 255)*32768.0; };

// The architecture supplies both curves in address order during instance initialization.
rom_word = ffunction(int envelope_rom_word(int), "", "");
curve_word(index) = float(rdtable(131, rom_word((+(1) ~ _)-1), index))*256.0;
curve(base, address) = high(interpolate(upper, lower, address))
with {
    index = (int(address) & 16777215) >> 8;
    next = index + (1');
    lookup(i) = curve_word(select2(i + base < 130, 130, i + base));
    lower = lookup(index);
    upper = lookup(next);
};

highpass(x) = x : (accumulate ~ high)
with { accumulate(previous, input) = 8379644.0*(input - input') + 8370680.0*previous; };

detector(x) = (step ~ bus(3)) : (!, !, !, _)
with {
    step(previous, previous_peak, threshold_state) = envelope, peak, closed, target
    with {
        drive = multiply(high(highpass(x)), 442738.0);
        rectified = select2(drive < 0.0, drive, -drive - 1.0);
        decay_acc = previous_peak*8387769.0;
        decayed = high(decay_acc);
        envelope = high(rectified*unit + low(decay_acc) + previous*8106751.0);
        peak = max(decayed, envelope);
        threshold = select2(threshold_state, 2048.0, 16384.0);
        closed = envelope < threshold;
        target = select2(closed, select2(peak < limit, 65535.0,
            multiply(curve(0, multiply(decayed, 16128.0)), limit)), 655.0);
    };
};

tap_oscillator = (step ~ bus(2)) : \(x, y).(clip(x*cos(phase) + y*sin(phase)))
with {
    step(previous_x, previous_y) = x, y
    with {
        x = clip(previous_x + (1 - 1')*8388606.0 + multiply(previous_y, rate));
        y = clip(previous_y - multiply(x, rate));
    };
};

filter(gain, pole) = (step ~ _)
with { step(previous, input) = high(input*gain + previous*pole); };

short_return(mono, tank_return) = curve(65, address) : *(curve_gain) : high
    : filter(early_gain, -early_pole) : filter(early_gain, -early_pole)
    : filter(diffusion_pole, late_gain) : filter(diffusion_pole, late_gain)
    : filter(diffusion_pole, late_gain)
with {
    early_gain = full - filter_pole' - (1 - 1')*83886.0;
    early_pole = filter_pole' + (1 - 1')*83887.0;
    late_gain = (full - diffusion_pole') - (1 - 1')*6710887.0;
    position = clip(multiply(tap_oscillator, 45056.0) + 45056.0);
    lower = (int(position') >> 8);
    upper = lower + (1@2);
    // The oscillator bounds both taps to offsets 0–352 in the mono delay.
    tap(index) = ram(mono)@max(0, min(352, index));
    fractional_tap = high(interpolate(tap(upper), tap(lower), position'));
    target = detector(mono')' + (1 - 1')*65535.0;
    delay_gain_acc = (smooth ~ high)
    with { smooth(previous) = target*41942.0 + previous*8346665.0; };
    feedback_input = high(mono*4194304.0 + fractional_tap'*4194304.0);
    returned = high(feedback_input'*unit + low(delay_gain_acc) + tank_return*return_feedback);
    feedback_acc = highpass(returned);
    // The short return exposes a signed 24-bit accumulator fraction.
    tap_product = multiply(ram(high(feedback_acc))@446, high(delay_gain_acc)')*16.0;
    fraction_bits = 2.0*low(tap_product);
    signed_fraction = fraction_bits - select2(fraction_bits >= unit, 0.0, 2.0*unit);
    modulation = clip(clip(multiply(clip(signed_fraction*4.0), address_gain)*4.0)*4.0);
    // The preceding high-pass contributes its accumulator fraction to curve A.
    address = high(8064.0*unit + low(feedback_acc) + modulation'*8064.0);
};

// Delays connect each read to the preceding writer in the DSP's circular RAM.
diffusion_tank(input) = (step ~ bus(6)) : (!, !, !, !, !, !, _, _, _)
with {
    step(first_sum, first_return, second_sum, second_return, left_sum, left_return) =
        first_sum_next, first_return_next, second_sum_next, second_return_next,
        left_sum_next, left_return_next, left, right, combined
    with {
        first_decay = left_return@1865*decay_gain;
        first_tap = first_sum@772;
        first_sum_next = ram(high(first_decay + first_tap*allpass_gain));
        first_return_next = ram(high(-high(first_decay)*allpass_gain + first_tap*allpass_complement));
        second_decay = first_return@1493*decay_gain;
        second_tap = second_sum@1468;
        second_sum_next = ram(high(second_decay + second_tap*3932160.0));
        second_return_acc = -high(second_decay)*3932160.0 + second_tap*6545408.0;
        second_return_next = ram(high(second_return_acc));
        feedback = high(input*unit + low(second_return_acc) + second_return@798*decay_gain);
        tank_acc = (step_filter ~ high)
        with { step_filter(previous) = feedback*(full - tank_damp) + previous*tank_damp; };
        short = left_sum@401;
        left_sum_next = ram(high(tank_acc + short*allpass_gain));
        left_return_next = ram(high(-high(tank_acc)*allpass_gain + short*allpass_complement));
        left = high(first_sum@49*5872025.0 - second_sum@371*4697621.0 + left_sum@348*5158994.0);
        right_acc = left_return@449*6501171.0 - second_sum@1112*5400167.0 + second_return@339*3460300.0;
        right = high(right_acc);
        // The return to the short delay includes the fraction of the right mix.
        combined = high(half(left)*unit + low(right_acc) + right*4194304.0);
    };
};

program(dry_l, dry_r, fx_l, fx_r, send_l, send_r) = (step ~ bus(4)) : (!, !, !, !, _, _, _)
with {
    step(diffusion, tank_l, tank_r, tank_return) = diffusion_tank(send) : \(left, right, combined).(
        short_return(mono, tank_return), left, right, combined,
        output(dry_l, send_l, tank_l), output(dry_r, send_r, tank_r), peak)
    with {
        // Convert the OTIS serial words to Q23 and truncate the DAC return to 16 bits.
        counts(x) = 256.0*min(32767.0, max(-32768.0, int(float(x)*32768.0 + select2(x < 0, 0.5, -0.5))));
        mono = half(counts(fx_l)) + half(counts(fx_r));
        send_side(x) = high(diffusion*(fx_to_reverb*unit) + counts(x)*(send_to_reverb*unit));
        send = half(send_side(send_l)) + half(send_side(send_r));
        output(dry, reverb, wet) = ram(multiply(clip(clip(wet + counts(dry)) +
            high(diffusion*(fx_direct*unit) + counts(reverb)*(send_direct*unit))), 8388352.0))/unit;
        peak = max3(abs(diffusion), abs(tank_l), abs(tank_r))/unit;
    };
};

process = bus(6) : program;
