#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "eeprom.hpp"
#include "interpreter.hpp"
#include "third_party/audio/mc68681.hpp"
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char *why) { if(!ok)throw std::runtime_error(why); }
f3rt::RomSet fixture() {
    f3rt::RomSet r;
    r.main.resize(0x200000);r.sprites.resize(0x400000);r.sprites_hi.resize(0x200000);
    r.tiles.resize(0x400000);r.tiles_hi.resize(0x200000);r.sound.resize(0x80000);r.samples.resize(0x1000000);
    r.main[1]=0x41;r.main[2]=0xff;r.main[3]=0xf0; // SSP 41fff0
    r.main[6]=1; // PC 100
    r.main[0x100]=0x70;r.main[0x101]=0x2a; // moveq #42,d0
    r.main[0x102]=0x60;r.main[0x103]=0xfe; // bra self
    r.samples[0x2468a]=0x45;r.samples[0x2468b]=0x67; // OTIS word 0x12345, above old truncated mask
    return r;
}
void send_bit(f3rt::Eeprom &e,bool bit,uint64_t now) { uint8_t pins=0x10|(bit?4:0);e.pins(pins,now);e.pins(pins|8,now); }
void command(f3rt::Eeprom &e,unsigned word,uint64_t now) { e.pins(0,now);for(int bit=8;bit>=0;--bit)send_bit(e,(word>>bit)&1,now); }
void serial_write(f3rt::Eeprom &e,unsigned address,uint16_t value,uint64_t now) {
    command(e,0x140|address,now);for(int bit=15;bit>=0;--bit)send_bit(e,(value>>bit)&1,now);e.pins(0,now);
}
uint16_t read_word(f3rt::Eeprom &e,uint64_t now) { uint16_t value=0;for(int i=0;i<16;++i) { send_bit(e,false,now);value=uint16_t((value<<1)|e.output(now)); }return value; }
void native(f3_cpu *cpu) { cpu->d[0]=99;cpu->pc+=2;cpu->cycles+=4; }
void check_main_sound_ordering() {
    const auto sound_machine=[] {
        auto roms=fixture();
        roms.sound[2]=0x80;roms.sound[6]=1; // SSP 8000, PC 100.
        auto m=std::make_unique<f3rt::Machine>(std::move(roms));
        // Real sound CPU: snapshot mailbox byte zero, post a reply, then loop.
        const uint16_t code[]={0x13f9,0x0014,0x0000,0x0000,0x0600,
                               0x13fc,0x005a,0x0014,0x0002,0x60fe};
        for (unsigned i=0;i<std::size(code);++i) m->audio->write16(0x100+2*i,code[i]);
        m->write8(0xc00000,0x11);
        return m;
    };
    {
        auto m=sound_machine();
        m->cpu.cycles=2048;
        f3_write16(&m->cpu,0xc80000,0);
        m->boundary();
        require(m->audio->read8(0x600)==0 && m->shared[1]==0,
                "Sound reset release cannot execute the CPU during preceding main-block time");
        m->cpu.cycles+=1024;m->boundary();
        require(m->audio->read8(0x600)==0x11 && m->shared[1]==0x5a,
                "Sound CPU executes the mailbox program after reset release");
    }
    for (unsigned width : {1,2,4}) {
        auto m=sound_machine();
        m->audio->set_reset(false);m->cpu.cycles=1024;
        if (width==1) f3_write8(&m->cpu,0xc00000,0x22);
        else if (width==2) f3_write16(&m->cpu,0xc00000,0x2233);
        else f3_write32(&m->cpu,0xbfffff,0x99223344); // Unaligned access enters shared RAM.
        m->boundary();
        require(m->audio->read8(0x600)==0x11 && m->shared[0]==0x22,
                "Mailbox writes become visible only after preceding sound execution");
    }
    for (unsigned width : {1,2,4}) {
        auto m=sound_machine();
        m->audio->set_reset(false);m->cpu.cycles=1024;
        const uint32_t value=width==1?f3_read8(&m->cpu,0xc00001):
            width==2?f3_read16(&m->cpu,0xc00000):f3_read32(&m->cpu,0xbfffff);
        require(value==(width==1?0x5au:width==2?0x115au:0xff115a00u),
                "Mailbox reads observe sound replies produced before the current main instruction");
    }
    for (bool reset_instruction : {false,true}) {
        auto m=sound_machine();
        m->audio->set_reset(false);m->cpu.cycles=1024;
        if (reset_instruction) f3_reset_devices(&m->cpu);
        else f3_write8(&m->cpu,0xc80100,0);
        m->boundary();
        require(m->audio->is_reset() && m->audio->read8(0x600)==0x11 && m->shared[1]==0x5a,
                "Reset assertion preserves sound execution preceding the reset instruction");
    }
}
void check_duart_counter() {
    const auto preset=[](f3rt::MC68681 &d,unsigned count) { d.write(6,count>>8);d.write(7,count); };
    f3rt::MC68681 restart;
    preset(restart,3);restart.write(4,0x30);restart.read(14);restart.advance(17);
    restart.read(14);restart.advance(47);
    require((restart.read(5)&8)==0,"Restarting the counter discards the preceding divider phase");
    restart.advance(1);
    require((restart.read(5)&8)!=0 && restart.read(6)==0xff && restart.read(7)==0xff,
            "Counter expires at the new deadline and reloads the reference 0xffff period");
    restart.advance(16);
    require(restart.read(7)==0xfe,"Counter underflow reload is independent of the programmed preset");
    restart.read(15);restart.advance(1000000);
    require((restart.read(5)&8)==0,"Stop-counter read acknowledges and cancels counter-mode expiration");

    f3rt::MC68681 mode;
    preset(mode,3);mode.write(4,0x30);mode.read(14);mode.advance(17);mode.read(15);
    mode.write(4,0x60);mode.advance(5);
    require((mode.read(5)&8)==0,"Entering timer mode starts a fresh full period without old divider residue");
    mode.advance(1);
    require((mode.read(5)&8)!=0,"Timer ready asserts after both half-periods");
    mode.read(15);mode.advance(6);
    require((mode.read(5)&8)!=0,"Timer-mode acknowledgement does not stop periodic interrupts");

    f3rt::MC68681 source;
    preset(source,2);source.write(4,0x70);source.advance(8);source.write(4,0x60);
    source.advance(23);
    require(source.read(7)==1 && (source.read(5)&8)==0,"Clock-source change preserves the armed duration");
    source.advance(1);source.advance(1);
    require((source.read(5)&8)==0,"The next half-period uses the newly selected source");
    source.advance(1);
    require((source.read(5)&8)!=0,"Reload adopts the new clock without rescaling elapsed time");

    f3rt::MC68681 reset;
    reset.advance(1000000);
    require(reset.read(5)==0,"A cold DUART has no scheduled counter event");
    preset(reset,100);reset.write(4,0x60);reset.advance(37);reset.reset();
    require(reset.read(5)==0 && !reset.irq_pending(),"Board reset clears the visible IRQ registers");
    reset.advance(62);
    require(reset.read(5)==0,"Board reset preserves the remaining deadline, not a restarted period");
    reset.advance(1);
    require(reset.read(5)==8 && !reset.irq_pending(),"Retained expiration latches counter-ready while reset IMR masks IRQ");
    reset.write(12,64);reset.write(5,8);
    require(reset.irq_pending() && reset.get_irq_vector()==64 && reset.irq_pending(),
            "Unmasking retained counter-ready asserts IRQ; IACK supplies vector without clearing it");
    reset.read(15);
    require(!reset.irq_pending(),"Counter acknowledge clears a retained post-reset interrupt");
}
void check_duart_tx() {
    for (unsigned channel : {0,1}) {
        f3rt::MC68681 duart;
        const unsigned base=channel*8,half=channel?64:32;
        const uint8_t mask=channel?0x10:0x01;
        bool irq=false;
        duart.set_irq_callback([&](bool asserted) { irq=asserted; });
        const auto configure=[&] {
            duart.write(base,0x13);duart.write(base,0x0f); // 8N2
            duart.write(base+1,0xee); // F3 external clock / 16
            duart.write(5,mask);duart.write(base+2,4);
        };
        require((duart.read(base+1)&0x0c)==0,"Reset disables the UART transmitter");
        configure();
        require((duart.read(base+1)&0x0c)==0x0c && irq,"Enabling an idle transmitter asserts ready/empty and its IRQ");
        duart.write(base+3,0x80);
        require((duart.read(base+1)&0x0c)==0 && !irq,"THR write clears ready and empty");
        duart.advance(3*half-1);
        require((duart.read(base+1)&0x0c)==0,"TX ready waits through the start-bit time");
        duart.advance(1);
        require((duart.read(base+1)&0x0c)==4 && irq,"The second rising edge releases THR and asserts TX ready");
        duart.advance(18*half-1);
        require((duart.read(base+1)&8)==0,"TX empty stays clear until the last framed bit");
        duart.advance(1);
        require((duart.read(base+1)&0x0c)==0x0c,"First 8N2 frame completes after 21 half-bit clocks");
        duart.write(base+3,0x90);duart.advance(4*half);
        duart.write(base+3,0x7f);duart.write(base+3,0x44); // One holding slot; third byte is rejected.
        require((duart.read(base+1)&0x0c)==0 && !irq,"A queued byte occupies THR while the current byte shifts");
        duart.advance(18*half);
        require((duart.read(base+1)&0x0c)==4 && irq,"Buffered transfer restores ready without asserting empty");
        duart.advance(22*half-1);
        require((duart.read(base+1)&8)==0,"Queued frame retains the continuous serial edge phase");
        duart.advance(1);
        require((duart.read(base+1)&0x0c)==0x0c,"Holding-register overflow does not enqueue an extra frame");
        duart.reset();configure();duart.write(base+3,0x80);duart.advance(22*half-1);
        require((duart.read(base+1)&8)==0,"Board reset retains the stopped serial clock edge state");
        duart.advance(1);
        require((duart.read(base+1)&8)!=0,"Post-reset frame uses a full first bit period");
        duart.write(base+3,0x55);duart.advance(4*half);duart.write(base+2,0x30);duart.advance(100*half);
        require((duart.read(base+1)&0x0c)==0 && !irq,"Transmitter reset aborts the frame and clears its IRQ");
        duart.write(base+2,0x10);duart.write(base,0);duart.write(base,7); // 5E1
        duart.write(base+1,0xef);duart.write(base+2,4);duart.write(base+3,0x15);
        const unsigned fast_half=half/16;
        duart.advance(16*fast_half-1);
        require((duart.read(base+1)&8)==0,"Word length, parity and direct external clock determine frame duration");
        duart.advance(1);
        require((duart.read(base+1)&8)!=0,"5E1 frame completes at its eight-bit boundary");
    }
    f3rt::MC68681 counter_clock;
    counter_clock.write(6,0);counter_clock.write(7,1);counter_clock.write(4,0x60);
    counter_clock.write(8,0x13);counter_clock.write(8,0x0f);
    counter_clock.write(9,0xed);counter_clock.write(10,4);counter_clock.write(11,0x80);
    counter_clock.read(14);
    counter_clock.advance(46);
    require((counter_clock.read(9)&0x0c)==0,"Counter-derived TX clock retains the divide-by-16 prescaler");
    counter_clock.advance(1);
    require((counter_clock.read(9)&0x0c)==4,"Counter-derived TX ready follows the second serial rising edge");
    counter_clock.advance(287);
    require((counter_clock.read(9)&8)==0,"Counter-derived TX empty waits for the full frame");
    counter_clock.advance(1);
    require((counter_clock.read(9)&0x0c)==0x0c,"Counter-derived 8N2 frame completes at its eleventh serial edge");
}
void check_sound_cycles(f3rt::Machine &m) {
    struct Result { int cycles;uint32_t d0,a1;uint16_t sr; };
    const auto execute = [&](std::initializer_list<uint16_t> instruction,uint32_t source=0,uint32_t d0=0) {
        m.audio->set_reset(true);
        m.audio->write32(0,0xff00);m.audio->write32(4,0x1000);m.audio->write32(0x4000,source<<16);
        uint32_t pc=0x1000;
        const auto emit = [&](uint16_t word) { m.audio->write16(pc,word);pc+=2; };
        for (uint16_t word : {uint16_t(0x203c),uint16_t(d0>>16),uint16_t(d0),
                              uint16_t(0x223c),uint16_t(source>>16),uint16_t(source),
                              uint16_t(0x227c),uint16_t(0),uint16_t(0x4000),uint16_t(0x46fc),uint16_t(0x271b)})
            emit(word);
        for (auto word : instruction) emit(word);
        // Save SR before the MOVE instructions used to observe D0/A1 change flags.
        for (uint16_t word : {0x40f8,0x1508,0x21c0,0x1500,0x21c9,0x1504}) emit(word);
        m.audio->set_reset(false);
        for (int i=0;i<5;++i) m.interpreter->run_audio(1); // Reset plus register/SR setup.
        const int cycles=m.interpreter->run_audio(1);
        for (int i=0;i<3;++i) m.interpreter->run_audio(1);
        const Result result{cycles,m.audio->read32(0x1500),m.audio->read32(0x1504),m.audio->read16(0x1508)};
        m.audio->set_reset(true);
        return result;
    };
    const auto quick=execute({0x5049});
    require(quick.cycles==8 && quick.a1==0x4008 && quick.sr==0x271b,
            "68000 ADDQ.W to an address register takes eight cycles and preserves flags");
    require(execute({0x544f}).cycles==8,"68000 ADDQ.W stack adjustment has the same full cost");
    require(execute({0xd2fc,10}).cycles==12 && execute({0x92fc,10}).cycles==12,
            "68000 immediate word address arithmetic has no long-operand surcharge");
    require(execute({0xd3fc,0,10}).cycles==16 && execute({0x93fc,0,10}).cycles==16,
            "68000 immediate long address arithmetic retains its surcharge");
    for (uint16_t family : {0xd000,0x9000,0xc000,0x8000}) {
        require(execute({uint16_t(family|0x3c),1}).cycles==8 &&
                execute({uint16_t(family|0x7c),1}).cycles==8 &&
                execute({uint16_t(family|0xbc),0,1}).cycles==16,
                "68000 immediate EA arithmetic charges byte, word and long operands distinctly");
        require(execute({uint16_t(family|0x81)}).cycles==8,
                "68000 long register arithmetic takes eight cycles");
        if (family==0xd000 || family==0x9000)
            require(execute({uint16_t(family|0x89)}).cycles==8,"68000 long arithmetic accepts address-register sources");
    }
    for (uint16_t opcode : {0xd3c0,0xd3c8,0x93c0,0x93c8})
        require(execute({opcode}).cycles==8,"68000 long address arithmetic has an eight-cycle register cost");
    const auto tas=execute({0x4ad1});
    require(tas.cycles==14 && tas.sr==0x2714 && m.audio->read8(0x4000)==0x80,
            "68000 memory TAS charges its read-modify-write once and reports the original byte");
    require(execute({0x4ac0}).cycles==4 && execute({0x4ad9}).cycles==14 &&
            execute({0x4ae1}).cycles==16 && execute({0x4ae9,16}).cycles==18 &&
            execute({0x4af8,0x4000}).cycles==18 && execute({0x4af9,0,0x4000}).cycles==22,
            "68000 TAS retains register and effective-address timing distinctions");
    for (unsigned kind=1;kind<=3;++kind) for (unsigned bit : {0,2,15,16,31,32,47,48,63}) {
        const int cycles=(kind==2?8:6)+((bit&31)>=16?2:0);
        const uint32_t result=kind==2?0:1u<<(bit&31);
        const auto reg=execute({uint16_t(0x0300|(kind<<6))},bit);
        const auto immediate=execute({uint16_t(0x0800|(kind<<6)),uint16_t(bit)});
        require(reg.cycles==cycles && immediate.cycles==cycles+4,
                "68000 register bit mutations distinguish low/high halves after modulo-32 selection");
        require(reg.d0==result && immediate.d0==result && reg.sr==0x271f && immediate.sr==0x271f,
                "Bit timing preserves result, old-bit Z and untouched X/N/V/C flags");
    }
    const struct { uint32_t dividend;uint16_t divisor;int cycles; } divisions[]={
        {0,1,136},{0xffff,1,106},{0x10000,1,10},{0x10000,2,134},
        {0xff1234,0x100,116},{0x80000000,0xffff,132},{0xfffeffff,0xffff,76}
    };
    for (const auto &test : divisions) {
        const auto reg=execute({0x80c1},test.divisor,test.dividend);
        const auto immediate=execute({0x80fc,test.divisor},test.divisor,test.dividend);
        const auto memory=execute({0x80d1},test.divisor,test.dividend);
        require(reg.cycles==test.cycles && immediate.cycles==test.cycles+4 && memory.cycles==test.cycles+4,
                "68000 DIVU.W timing follows the operand-dependent subtract stages and EA cost");
        const auto quotient=test.dividend/test.divisor;
        const bool overflow=quotient>0xffff;
        const auto result=overflow?test.dividend:((test.dividend%test.divisor)<<16)|quotient;
        const uint16_t flags=0x10|(overflow?2:((quotient==0?4:0)|(quotient&0x8000?8:0)));
        const uint16_t mask=overflow?0x13:0x1f; // N/Z are undefined on overflow.
        require(reg.d0==result && immediate.d0==result && memory.d0==result &&
                (reg.sr&mask)==flags && (immediate.sr&mask)==flags && (memory.sr&mask)==flags,
                "DIVU timing preserves packed remainder/quotient, overflow destination and defined flags");
    }
}
void check_sound_irq(f3rt::Machine &m) {
    for (unsigned vector : {15,30,64,255}) {
        m.audio->reset_board();
        m.audio->write32(0,0xff00);m.audio->write32(4,0x1000);m.audio->write32(vector*4,0x2000);
        m.audio->write16(0x1000,0x4e72);m.audio->write16(0x1002,0x2000);m.audio->write16(0x2000,0x4e71);
        m.audio->set_reset(false);m.interpreter->run_audio(1);m.interpreter->run_audio(1);
        m.audio->write8(0x280019,vector);m.audio->write8(0x28000d,0);m.audio->write8(0x28000f,1);
        m.audio->write8(0x28000b,8);m.audio->write8(0x280009,0x60);m.audio->advance(8);
        require(m.audio->irq_level()==6,"DUART timer wakes the stopped sound CPU on IRQ6");
        require(m.interpreter->run_audio(1)==48 && m.interpreter->sound_pc()==0x2002,
                "68000 IRQ entry costs 44 cycles independently of vector number, plus the handler NOP");
        require(m.audio->read16(0xfefa)==0x2000 && m.audio->read32(0xfefc)==0x1004,
                "Vectored IRQ entry preserves the stopped CPU's SR and return PC");
    }
    m.audio->reset_board();
}
void check_trap_cycles(f3rt::Machine &m) {
    const auto old_vbr=m.cpu.vbr;
    for (unsigned trap=0;trap<16;++trap) for (bool reference : {false,true}) {
        m.cpu.pc=0x400700;m.cpu.sr=0x2700;m.cpu.a[7]=0x401000;m.cpu.vbr=0x400000;
        m.write16(0x400700,uint16_t(0x4e40|trap));
        m.write32(m.cpu.vbr+(32+trap)*4,0x400720);
        const auto before=m.cpu.cycles;
        if (reference) m.interpreter->run_main(1);
        else f3_exception(&m.cpu,32+trap,0x400702);
        require(m.cpu.cycles-before==24 && m.cpu.pc==0x400720,
                "TRAP #n charges 24 cycles before entering its vector");
        require(m.cpu.a[7]==0x400ff8 && m.read16(m.cpu.a[7])==0x2700 &&
                m.read32(m.cpu.a[7]+2)==0x400702 && m.read16(m.cpu.a[7]+6)==(32+trap)*4,
                "TRAP #n stacks the resume PC in a format-0 frame");
    }
    m.cpu.vbr=old_vbr;
}
void check_movem(f3rt::Machine &m) {
    for (uint16_t opcode : {0x4891,0x48d1,0x48a1,0x48e1,0x4c99,0x4cd9}) {
        const bool load=opcode&0x0400, wide=opcode&0x0040, predec=(opcode&0x0038)==0x20;
        const unsigned size=wide?4:2;
        const auto execute = [&](uint16_t mask) {
            m.write16(0x400600,opcode);m.write16(0x400602,mask);
            m.cpu.pc=0x400600;m.cpu.sr=0x2700;m.cpu.a[1]=0x400a20;
            m.cpu.d[0]=0x12345678;m.cpu.d[1]=0x89abcdef;
            if (load) {
                if (wide) { m.write32(0x400a20,0x12345678);m.write32(0x400a24,0x89abcdef); }
                else { m.write16(0x400a20,0x5678);m.write16(0x400a22,0xcdef); }
            }
            const auto before=m.cpu.cycles;
            m.interpreter->run_main(1);
            require(m.cpu.pc==0x400604,"MOVEM executes exactly one instruction");
            return m.cpu.cycles-before;
        };
        const auto base=execute(0);
        const auto cycles=execute(predec?0xc000:3);
        require(cycles-base==(load?8u:6u),"EC020 MOVEM charges three cycles/store and four/load per register");
        if (load) {
            require(m.cpu.d[0]==(wide?0x12345678u:0x5678u) &&
                    m.cpu.d[1]==(wide?0x89abcdefu:0xffffcdefu) &&
                    m.cpu.a[1]==0x400a20+2*size,"MOVEM load width, sign extension and postincrement");
        } else {
            const uint32_t address=predec?0x400a20-2*size:0x400a20;
            require((wide?m.read32(address):m.read16(address))==(wide?0x12345678u:0x5678u) &&
                    (wide?m.read32(address+size):m.read16(address+size))==(wide?0x89abcdefu:0xcdefu) &&
                    m.cpu.a[1]==address,"MOVEM store width, register order and predecrement");
        }
    }
}
void check_rotate_cycles(f3rt::Machine &m) {
    const auto execute = [&](uint16_t opcode,unsigned count) {
        m.write16(0x400700,opcode);
        m.cpu.pc=0x400700;m.cpu.sr=0x2710;m.cpu.d[0]=0x12345678;m.cpu.d[1]=count;
        const auto before=m.cpu.cycles;
        m.interpreter->run_main(1);
        require(m.cpu.pc==0x400702,"Shift/rotate executes one instruction");
        return m.cpu.cycles-before;
    };
    for (unsigned kind=0;kind<4;++kind) for(unsigned left=0;left<2;++left) for(unsigned size=0;size<3;++size) {
        const uint16_t form=uint16_t(0xe000|(kind<<3)|(left<<8)|(size<<6));
        const uint16_t reg=uint16_t(form|0x0220);
        const auto base=execute(reg,0);
        for(unsigned count : {1,8,9,16,17,31,32,33,63})
            require(execute(reg,count)==base,"EC020 register shifts/rotates have no count-dependent cycle surcharge");
        require(execute(uint16_t(form|0x0200),0)==execute(form,0),
                "EC020 immediate counts one and eight have identical timing");
    }
    execute(0xe898,0); // ROR.L #4,D0, used by the ROM's early boot path.
    require(m.cpu.d[0]==0x81234567 && (m.cpu.sr&0x1f)==0x19,"ROR.L result, carry and preserved extend flag");
}
void check_audio_mixer() {
    f3rt::Audio audio;
    const std::array<uint8_t,4> rom{0x40,0,0x40,0};
    audio.load_sample_rom(rom);
    audio.write16(0x20001e,0x20);
    for (unsigned reg=1;reg<=6;++reg) audio.write16(0x200000+reg*2,0x4000);
    audio.write16(0x20001e,0);
    audio.write16(0x200010,0xff00);audio.write16(0x200012,0xff00);
    audio.write16(0x200000,0x0c00); // Constant sample, all poles lowpass, auxiliary pair.
    std::array<int16_t,2> pcm{};
    const auto sample = [&] {
        audio.advance(538);
        require(audio.render(pcm.data(),1)==1,"One complete audio sample is available");
    };
    sample();
    // DC 0x4000, OTIS volume 15.5, /2^19, board 0.18, two 100/32
    // gain stages, auxiliary route 0.5, and signed PCM scale 32768.
    require(pcm[0]==13950 && pcm[1]==13950,"Board gain and signed PCM normalization");
    audio.write8(0x340000,2);audio.write8(0x340002,0);
    sample();
    require(pcm[0]==0 && pcm[1]==13950,"Left volume mute preserves the right channel");
    audio.write8(0x340000,7);audio.write8(0x340002,0x33);
    sample();
    require(pcm[0]==0 && pcm[1]==3487,"Minus-six-dB control applies both baseline gain stages");
    audio.set_gain_model(f3rt::Audio::GainModel::SingleStage);
    sample();
    require(pcm[0]==0 && pcm[1]==715,"Single-stage gain is distinct and preserves channel mute");
    audio.set_reset(false);audio.set_reset(true);
    sample();
    require(pcm[0]==0 && pcm[1]==715,"CPU-line reset preserves attenuation and playing OTIS voices");
    audio.reset_board();
    sample();
    require(pcm[0]==1428 && pcm[1]==1428,"Board reset restores volume without resetting OTIS voices");
}
}
int main() try {
    check_audio_mixer();
    check_main_sound_ordering();
    f3rt::Audio clock_audio;
    std::array<int16_t, 128> clock_samples{};
    uint64_t sample_count=0;
    for (unsigned i=0;i<10000;++i) {
        clock_audio.advance(16000);
        sample_count+=clock_audio.render(clock_samples.data(),clock_samples.size()/2);
    }
    require(sample_count==uint64_t(clock_audio.sample_rate())*10,
            "Ten seconds of audio match the advertised stream rate without clock drift");
    clock_audio.advance(638);
    clock_audio.reset_board();
    clock_audio.advance(438);
    require(clock_audio.render(clock_samples.data(),clock_samples.size()/2)==2,
            "Board reset preserves queued audio and fractional sample-clock phase");
    auto m=std::make_unique<f3rt::Machine>(fixture());
    require(m->cpu.cycles==4 && m->cpu.pc==0x100 && m->cpu.d[0]==0,
            "Cold reset charges four cycles without executing the first opcode");
    require(m->cpu.dispatch_deadline==0,"Reset requires a fresh scheduling boundary");
    const auto raster_tick = [](uint64_t pixels) {
        return (pixels*f3rt::Machine::main_clock+f3rt::Machine::pixel_clock-1)/f3rt::Machine::pixel_clock;
    };
    const auto boundary_at = [&](uint64_t tick) { m->cpu.cycles=tick;m->boundary(); };
    const auto first_vblank=raster_tick(f3rt::Machine::frame_pixels);
    boundary_at(first_vblank-1);
    require(m->frame==0 && m->pending_irqs==0,"First vblank waits one full frame from the VBSTART epoch");
    require(m->cpu.dispatch_deadline==first_vblank,"Native deadline is the first vblank event");
    boundary_at(first_vblank);
    require(m->frame==1 && m->pending_irqs==(1<<2),"First vblank renders and requests IRQ2 at its deadline");
    require(m->cpu.dispatch_deadline==first_vblank+10000,"Delayed IRQ3 becomes the next native deadline");
    boundary_at(first_vblank+9999);
    require(m->pending_irqs==(1<<2),"IRQ3 is not requested before its 10000-cycle delay");
    boundary_at(first_vblank+10000);
    require(m->pending_irqs==((1<<2)|(1<<3)),"IRQ3 is requested at its delayed deadline");
    require(m->cpu.dispatch_deadline==raster_tick(2ull*f3rt::Machine::frame_pixels),
            "Serviced timer deadlines advance to the next absolute frame");
    m->pending_irqs=0;
    const auto second_vblank=raster_tick(2ull*f3rt::Machine::frame_pixels);
    boundary_at(second_vblank-1);
    require(m->frame==1 && m->pending_irqs==0,"Next vblank retains the absolute raster phase");
    boundary_at(second_vblank);
    require(m->frame==2 && m->pending_irqs==(1<<2),"Second vblank uses the full-frame epoch");
    m->pending_irqs=0;
    check_movem(*m);
    check_rotate_cycles(*m);
    check_trap_cycles(*m);
    check_sound_cycles(*m);
    check_sound_irq(*m);
    check_duart_tx();
    check_duart_counter();
    m->write32(0x400001,0x12345678);
    require(m->read32(0x420001)==0x12345678,"BE misaligned work RAM mirror");
    m->write32(0x41fffe,0xaabbccdd);
    require(m->read16(0x400000)==0xccdd && m->read16(0x41fffe)==0xaabb,"Work RAM wrap across mirror");
    m->write8(0x100,0xff);require(m->read16(0x100)==0x702a,"ROM is read-only");
    m->write8(0xc00010,0x75);require(m->audio->read16(0x140020)==0x75ff,"DPRAM sound high-byte lane");
    m->audio->write16(0x140020,0xaabb);require(m->read8(0xc00010)==0xaa,"DPRAM reverse lane");
    m->audio->write16(0x20001e,0); // Voice 0, register page
    m->audio->write16(0x200014,0x0246);m->audio->write16(0x200016,0x8a00);
    m->audio->write16(0x20001e,0x20); // Stopped voice sample-ROM readback
    require(m->audio->read16(0x20000c)==0x4567,"OTIS stopped voice reads full 20-bit sample address");
    m->audio->write16(0x600,0xa55a);m->audio->set_reset(false);
    m->audio->set_reset(true);m->audio->set_reset(false);
    require(m->audio->read16(0x600)==0xa55a,"Sound CPU RESET preserves board work RAM");
    m->write8(0x4a0004,0x04);m->write8(0x4a0004,0x04);require(m->coin_count[0]==1,"Coin counter rising-edge only");
    m->set_input(0,0x1000,true);require(!(m->read32(0x4a0000)&0x1000),"Active-low start input");
    f3rt::Eeprom e;
    uint64_t now=100;
    serial_write(e,63,0x1234,now);require(e.words[63]==0xffff,"EEPROM write disabled at power-on");
    command(e,0x130,now);e.pins(0,now); // EWEN
    serial_write(e,63,0x1234,now);
    require(e.output(now),"Deselected EEPROM DO is pulled high while programming");
    e.pins(0x10,now);
    require(!e.output(now),"Raising CS exposes EEPROM programming busy");
    serial_write(e,0,0xdead,now);
    require(e.words[0]==0xffff,"EEPROM ignores new commands while programming");
    e.pins(0x10,now);
    now+=27999;require(!e.output(now),"EEPROM write remains busy before 1750us deadline");
    ++now;require(e.output(now),"EEPROM write finishes without another clock edge");
    serial_write(e,0,0xabcd,now);now+=28000;
    command(e,0x1bf,now);require(!e.output(now),"EEPROM read dummy bit");
    require(read_word(e,now)==0x1234 && read_word(e,now)==0xabcd,"EEPROM sequential read wraps 63 to 0");
    e.pins(0,now);command(e,0x100,now);e.pins(0,now);serial_write(e,0,0x4321,now);
    e.pins(0x10,now);
    require(e.words[0]==0xabcd && e.output(now),"EEPROM EWDS protects contents without becoming busy");
    command(e,0x130,now);command(e,0x1ff,now); // EWEN; erase word 63
    e.pins(0,now);e.pins(0x10,now);
    now+=15999;require(!e.output(now),"EEPROM erase remains busy before 1000us deadline");
    ++now;require(e.output(now) && e.words[63]==0xffff,"EEPROM single-word erase completes");
    command(e,0x120,now);e.pins(0,now);e.pins(0x10,now); // ERAL
    now+=127999;require(!e.output(now),"EEPROM erase-all remains busy before 8000us deadline");
    ++now;require(e.output(now) && e.words[0]==0xffff,"EEPROM erase-all completes");
    command(e,0x110,now); // WRAL
    for(int bit=15;bit>=0;--bit)send_bit(e,(0x5a5a>>bit)&1,now);
    e.pins(0,now);e.pins(0x10,now);
    now+=127999;require(!e.output(now),"EEPROM write-all remains busy before 8000us deadline");
    ++now;require(e.output(now) && e.words[0]==0x5a5a && e.words[63]==0x5a5a,"EEPROM write-all completes");
    e.reset();e.pins(0x10,0);
    require(e.output(0) && e.words[0]==0x5a5a,"Power reset clears serial timing but preserves EEPROM contents");
    auto &cpu=m->cpu;
    cpu.usp=0x400800;cpu.a[7]=0x401000;cpu.sr=0x2600;
    require(m->boundary()==0 && cpu.dispatch_deadline>cpu.cycles,"Runnable boundary publishes a future deadline");
    const auto masked_deadline=cpu.dispatch_deadline;
    f3_set_sr(&cpu,0x2715);
    require(cpu.dispatch_deadline==masked_deadline,"Raising the IRQ mask preserves the event deadline");
    f3_set_sr(&cpu,0);require(cpu.a[7]==0x400800 && cpu.ssp==0x401000,"Supervisor to user stack switch");
    require(cpu.dispatch_deadline==0,"Lowering the IRQ mask invalidates the cached deadline");
    m->write8(0x4a0000,0);
    const auto watchdog_deadline=cpu.cycles+3ull*f3rt::Machine::main_clock;
    require(cpu.dispatch_deadline==0,"Watchdog strobe cannot suppress an outstanding IRQ recheck");
    m->write32(0x400000+26*4,0x400300);cpu.vbr=0x400000;cpu.pc=0x100;cpu.stopped=1;
    m->pending_irqs=1<<2;require(f3_boundary(&cpu)!=0,"IRQ redirects boundary");
    require(cpu.pc==0x400300 && !cpu.stopped && (cpu.sr&0x2700)==0x2200,"IRQ releases STOP and raises mask");
    require(cpu.dispatch_deadline==0,"IRQ redirect requires lookup through a fresh boundary");
    require(cpu.a[7]==0x400ff8 && m->read32(cpu.a[7]+2)==0x100 && m->read16(cpu.a[7]+6)==104,"68020 interrupt frame");
    m->write32(cpu.vbr+5*4,0x400310);m->write16(0x400310,0x4e73); // RTE handler
    cpu.pc=0x100;cpu.sr=0x2700;
    const auto exception_cycles=cpu.cycles;const auto exception_sp=cpu.a[7];
    f3_exception(&cpu,5,0x102);
    require(cpu.cycles-exception_cycles==38 && m->read16(cpu.a[7]+6)==0x2014 &&
            m->read32(cpu.a[7]+8)==0x100,"Divide-by-zero full charge and format-2 instruction PC");
    require(f3_fallback(&cpu) && cpu.pc==0x102 && cpu.a[7]==exception_sp && cpu.sr==0x2700,
            "Real RTE restores the format-2 resume PC and stack");
    require(m->boundary()==0 && cpu.dispatch_deadline>cpu.cycles,"Restored masked CPU refreshes its event deadline");
    m->write16(0x400320,0x46fc);m->write16(0x400322,0x2000);cpu.pc=0x400320;
    m->interpreter->run_main(1);
    require(cpu.sr==0x2000 && cpu.dispatch_deadline==0,"Interpreted SR lowering also invalidates the native deadline");
    const f3_block blocks[]={{0x100,native}};
    require(f3_register_blocks(&cpu,blocks,1)==1,"Valid block table");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_dispatch(&cpu) && cpu.d[0]==99,"Native dispatch executes matching block");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_fallback(&cpu) && cpu.d[0]==42 && cpu.pc==0x102,"Fallback executes exactly one real instruction");
    cpu.d[0]=0xdeadbeef;cpu.sr=0x2015;
    const auto reset_cycles=cpu.cycles;
    m->interpreter->reset_main();
    require(cpu.d[0]==0xdeadbeef && cpu.sr==0x2715 && cpu.pc==0x100,
            "Reset preserves canonical native D/CCR, not stale fallback context");
    require(cpu.cycles-reset_cycles==4,"Warm reset charges its four-cycle latency exactly once");
    m->allow_main_fallback=false;const auto fallback_count=m->fallback_instructions;
    bool rejected=false;
    try { f3_fallback(&cpu); } catch(const std::runtime_error &e) { rejected=std::string(e.what()).find("0x100")!=std::string::npos; }
    require(rejected && cpu.pc==0x100 && cpu.d[0]==0xdeadbeef && m->fallback_instructions==fallback_count,
            "Native-only fallback rejection reports PC without executing or counting an instruction");
    const f3_block bad[]={{0x100,native},{0x100,native}};
    require(!f3_register_blocks(&cpu,bad,2),"Duplicate PCs rejected");
    m->audio->set_reset(true);
    m->audio->write8(0x280019,0x66);
    m->audio->write8(0x260001,0x12);m->audio->write8(0x260003,0x34);
    m->audio->write8(0x260005,0x56);m->audio->write8(0x260141,0);
    m->audio->set_reset(false);m->audio->set_reset(true);
    m->audio->write8(0x260101,0);
    require(m->audio->read8(0x260001)==0x12 && m->audio->read8(0x280019)==0x66,
            "CPU-line reset preserves DSP registers and DUART configuration");
    m->audio->write32(0,0xabcdef12);
    cpu.sr=0x2700;cpu.cycles=watchdog_deadline-1;
    require(m->boundary()==0 && cpu.dispatch_deadline==watchdog_deadline,
            "Watchdog expiry can precede the next raster interrupt");
    cpu.stopped=1;
    require(m->boundary()!=0 && cpu.cycles==watchdog_deadline && cpu.dispatch_deadline==0,
            "STOP advances to watchdog expiry without skipping it");
    require(m->boundary()!=0 && m->audio->is_reset(),"Watchdog holds the sound CPU in reset");
    require(cpu.dispatch_deadline==0,"Watchdog reset invalidates the native deadline");
    require(m->audio->read8(0x280019)==0x0f,"Watchdog restores the DUART interrupt vector");
    require(m->audio->read16(0x600)==0xa55a,"Whole-board reset preserves sound work RAM");
    require(m->audio->read32(0)==0,"Whole-board reset reloads boot vectors from sound ROM");
    m->audio->write8(0x260101,0);
    require(m->audio->read8(0x260001)==0 && m->audio->read8(0x260003)==0 &&
            m->audio->read8(0x260005)==0,"Watchdog clears DSP general-purpose registers");
    std::cout<<"PASS memory/lanes, input/coin, EEPROM protocol, IRQ/stack, native dispatch and real interpreter\n";
    return 0;
} catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
