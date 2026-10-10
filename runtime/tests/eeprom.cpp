#include "f3rt/machine.hpp"
#include "eeprom.hpp"
#include "support.hpp"
#include <chrono>
#include <filesystem>

using f3test::fixture;
using f3test::require;
namespace {
void send_bit(f3rt::Eeprom &e,bool bit,uint64_t now) { uint8_t pins=0x10|(bit?4:0);e.pins(pins,now);e.pins(pins|8,now); }
void command(f3rt::Eeprom &e,unsigned word,uint64_t now) { e.pins(0,now);for(int bit=8;bit>=0;--bit)send_bit(e,(word>>bit)&1,now); }
void serial_write(f3rt::Eeprom &e,unsigned address,uint16_t value,uint64_t now) {
    command(e,0x140|address,now);for(int bit=15;bit>=0;--bit)send_bit(e,(value>>bit)&1,now);e.pins(0,now);
}
uint16_t read_word(f3rt::Eeprom &e,uint64_t now) { uint16_t value=0;for(int i=0;i<16;++i) { send_bit(e,false,now);value=uint16_t((value<<1)|e.output(now)); }return value; }
void send_bit(f3rt::Machine &m,bool bit) {
    const uint8_t pins=0x10|(bit?4:0);
    m.write8(0x4a0013,pins);m.write8(0x4a0013,pins|8);
}
void command(f3rt::Machine &m,unsigned word) {
    m.write8(0x4a0013,0);for(int bit=8;bit>=0;--bit)send_bit(m,(word>>bit)&1);
}
void serial_write(f3rt::Machine &m,unsigned address,uint16_t value) {
    command(m,0x140|address);
    for(int bit=15;bit>=0;--bit)send_bit(m,(value>>bit)&1);
    m.write8(0x4a0013,0);
}
uint16_t read_word(f3rt::Machine &m) {
    uint16_t value=0;
    for(int i=0;i<16;++i) {
        send_bit(m,false);value=uint16_t((value<<1)|(m.read8(0x4a0000)&1));
    }
    return value;
}
void check_factory_eeprom() {
    auto roms=fixture();
    roms.factory_eeprom.assign(128,0xff);
    roms.factory_eeprom[0]=0x12;roms.factory_eeprom[1]=0x34;
    roms.factory_eeprom[2]=0x89;roms.factory_eeprom[3]=0xab;
    roms.factory_eeprom[126]=0xfe;roms.factory_eeprom[127]=0xdc;
    auto m=std::make_unique<f3rt::Machine>(std::move(roms));
    command(*m,0x1bf);
    require(!(m->read8(0x4a0000)&1),"Factory EEPROM read exposes the serial dummy bit through the input port");
    require(read_word(*m)==0xfedc && read_word(*m)==0x1234 && read_word(*m)==0x89ab,
            "Factory EEPROM seeds all addresses big-endian before initial reset, including serial wrap");
    command(*m,0x130);m->write8(0x4a0013,0); // EWEN
    serial_write(*m,0,0xa65c);m->cpu.cycles+=28000;
    m->reset();command(*m,0x180);
    require(read_word(*m)==0xa65c && read_word(*m)==0x89ab,
            "Machine reset preserves guest EEPROM writes instead of reseeding factory defaults");

    struct TemporaryImage {
        std::filesystem::path path;
        ~TemporaryImage() { std::error_code error;std::filesystem::remove(path,error); }
    } image{std::filesystem::temp_directory_path()/
        ("f3rt-check-eeprom-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".nv")};
    f3rt::Eeprom user;
    user.words.fill(0x579b);user.words[0]=0xc318;user.words[63]=0x2468;
    user.save(image.path); // Only this explicitly chosen temporary user path is written.
    m->load_eeprom(image.path);m->reset();command(*m,0x1bf);
    require(read_word(*m)==0x2468 && read_word(*m)==0xc318 && read_word(*m)==0x579b,
            "Explicit user EEPROM image overrides factory and prior guest contents across reset");
    command(*m,0x130);m->write8(0x4a0013,0);
    serial_write(*m,0,0xd42e);m->cpu.cycles+=28000;
    m->reset();command(*m,0x180);
    require(read_word(*m)==0xd42e,"Loaded user EEPROM remains writable and persists through reset");
    roms=std::move(m->roms);m.reset();
    auto factory_machine=std::make_unique<f3rt::Machine>(std::move(roms));
    command(*factory_machine,0x1bf);
    require(read_word(*factory_machine)==0xfedc && read_word(*factory_machine)==0x1234 &&
            read_word(*factory_machine)==0x89ab,
            "A new machine consumes the unchanged ROM seed after user-image load and guest writes");
}
void check_eeprom_protocol() {
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
}
}
int main() {
    return f3test::run("EEPROM factory image and serial protocol",[] {
        check_factory_eeprom();
        check_eeprom_protocol();
    });
}
