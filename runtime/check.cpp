#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "eeprom.hpp"
#include "interpreter.hpp"
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
void send_bit(f3rt::Eeprom &e,bool bit) { uint8_t pins=0x10|(bit?4:0);e.pins(pins);e.pins(pins|8); }
void command(f3rt::Eeprom &e,unsigned word) { e.pins(0);for(int bit=8;bit>=0;--bit)send_bit(e,(word>>bit)&1); }
void serial_write(f3rt::Eeprom &e,unsigned address,uint16_t value) {
    command(e,0x140|address);for(int bit=15;bit>=0;--bit)send_bit(e,(value>>bit)&1);e.pins(0);
}
uint16_t read_word(f3rt::Eeprom &e) { uint16_t value=0;for(int i=0;i<16;++i) { send_bit(e,false);value=uint16_t((value<<1)|e.output()); }return value; }
void native(f3_cpu *cpu) { cpu->d[0]=99;cpu->pc+=2;cpu->cycles+=4; }
}
int main() try {
    auto m=std::make_unique<f3rt::Machine>(fixture());
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
    serial_write(e,63,0x1234);require(e.words[63]==0xffff,"EEPROM write disabled at power-on");
    command(e,0x130);e.pins(0); // EWEN
    serial_write(e,63,0x1234);serial_write(e,0,0xabcd);
    command(e,0x1bf);require(!e.output(),"EEPROM read dummy bit");
    require(read_word(e)==0x1234 && read_word(e)==0xabcd,"EEPROM sequential read wraps 63 to 0");
    e.pins(0);command(e,0x100);e.pins(0);serial_write(e,0,0x4321);require(e.words[0]==0xabcd,"EEPROM EWDS protects contents");
    auto &cpu=m->cpu;
    cpu.usp=0x400800;cpu.a[7]=0x401000;cpu.sr=0x2000;
    f3_set_sr(&cpu,0);require(cpu.a[7]==0x400800 && cpu.ssp==0x401000,"Supervisor to user stack switch");
    m->write32(0x400000+26*4,0x400300);cpu.vbr=0x400000;cpu.pc=0x100;cpu.stopped=1;
    m->pending_irqs=1<<2;require(f3_boundary(&cpu)!=0,"IRQ redirects boundary");
    require(cpu.pc==0x400300 && !cpu.stopped && (cpu.sr&0x2700)==0x2200,"IRQ releases STOP and raises mask");
    require(cpu.a[7]==0x400ff8 && m->read32(cpu.a[7]+2)==0x100 && m->read16(cpu.a[7]+6)==104,"68020 interrupt frame");
    const f3_block blocks[]={{0x100,native}};
    require(f3_register_blocks(&cpu,blocks,1)==1,"Valid block table");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_dispatch(&cpu) && cpu.d[0]==99,"Native dispatch executes matching block");
    cpu.pc=0x100;cpu.sr=0x2700;require(f3_fallback(&cpu) && cpu.d[0]==42 && cpu.pc==0x102,"Fallback executes exactly one real instruction");
    cpu.d[0]=0xdeadbeef;cpu.sr=0x2015;
    m->interpreter->reset_main();
    require(cpu.d[0]==0xdeadbeef && cpu.sr==0x2715 && cpu.pc==0x100,
            "Reset preserves canonical native D/CCR, not stale fallback context");
    m->allow_main_fallback=false;const auto fallback_count=m->fallback_instructions;
    bool rejected=false;
    try { f3_fallback(&cpu); } catch(const std::runtime_error &e) { rejected=std::string(e.what()).find("0x100")!=std::string::npos; }
    require(rejected && cpu.pc==0x100 && cpu.d[0]==0xdeadbeef && m->fallback_instructions==fallback_count,
            "Native-only fallback rejection reports PC without executing or counting an instruction");
    const f3_block bad[]={{0x100,native},{0x100,native}};
    require(!f3_register_blocks(&cpu,bad,2),"Duplicate PCs rejected");
    std::cout<<"PASS memory/lanes, input/coin, EEPROM protocol, IRQ/stack, native dispatch and real interpreter\n";
    return 0;
} catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
