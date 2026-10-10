#include "f3rt/machine.hpp"
#include "interpreter.hpp"
#include "state_io.hpp"
#include "sprite_units.hpp"
#include "renderer/sprite_behaviour.hpp"
#include "support.hpp"
#include <algorithm>
#include <array>
#include <memory>

using f3test::fixture;
using f3test::require;
namespace {
// Emit-unit replay fixtures: unit 0 bumps a RAM counter and draws entries 3-4 of bank 0;
// unit 1 touches I/O and must abort. Blocks follow the generated hook order (hook first).
constexpr uint32_t synth_starts0[]={0x100}, synth_ends0[]={0x108};
constexpr uint32_t synth_starts1[]={0x200}, synth_ends1[]={0x208};
constexpr f3rt::EmitUnit synth_unit0{0,"synth_bump",synth_starts0,synth_ends0,f3rt::UnitRegister::A3,0x20,f3rt::UnitOwner::Unit};
constexpr f3rt::EmitUnit synth_unit1{1,"synth_io",synth_starts1,synth_ends1,f3rt::UnitRegister::A3,0,f3rt::UnitOwner::Unit};
constexpr std::array<const f3rt::EmitUnit *,2> synth_units{&synth_unit0,&synth_unit1};
F3RT_SPRITE_BEHAVIOUR(synth_bump_behaviour,synth_unit0,"Adds 5 to the unit's first word",
    [](const f3rt::Unit<synth_unit0> &u,f3rt::Patch<synth_unit0> &p) {
        p.u16<0>(uint16_t(u.u16<0>()+5));
        return true;
    });
void synth_start(f3_cpu *cpu) {
    f3_unit_enter(cpu,0);
    const uint16_t v=f3_read16(cpu,cpu->a[3]);
    f3_write16(cpu,cpu->a[3]+2,uint16_t(v+1));
    f3_write16(cpu,0x600030,0xabcd);f3_write16(cpu,0x600032,v);
    cpu->pc=0x104;cpu->cycles+=8;
}
void synth_mid(f3_cpu *cpu) { f3_write16(cpu,0x600040,0x1111);cpu->pc=0x108;cpu->cycles+=4; }
void synth_end(f3_cpu *cpu) { if(f3_unit_exit(cpu,0))return;cpu->pc=0x10a;cpu->cycles+=4; }
void synth_io(f3_cpu *cpu) { f3_unit_enter(cpu,1);cpu->d[2]=f3_read32(cpu,0x4a0000);cpu->pc=0x204;cpu->cycles+=4; }
void synth_io_mid(f3_cpu *cpu) { cpu->pc=0x208;cpu->cycles+=4; }
void synth_io_end(f3_cpu *cpu) { if(f3_unit_exit(cpu,1))return;cpu->pc=0x20a;cpu->cycles+=4; }
const f3_block synth_blocks[]={{0x100,synth_start},{0x104,synth_mid},{0x108,synth_end},
                              {0x200,synth_io},{0x204,synth_io_mid},{0x208,synth_io_end}};
std::unique_ptr<f3rt::Machine> synth_machine(bool units,bool check,bool behaviour) {
    auto m=std::make_unique<f3rt::Machine>(fixture());
    m->allow_main_fallback=false;
    require(f3_register_blocks(&m->cpu,synth_blocks,std::size(synth_blocks)),"Synthetic unit blocks register");
    if(units) {
        f3rt::SpriteUnits::Options options;
        options.check=check;
        if(behaviour)options.behaviours.push_back(&synth_bump_behaviour);
        m->sprite_units=std::make_unique<f3rt::SpriteUnits>(*m,f3rt::SpriteUnitTable{synth_units,{}},std::move(options));
    }
    m->write16(0x400100,0x1234);
    m->cpu.sr=0x2700;m->cpu.a[3]=0x400100;
    return m;
}
void synth_span(f3rt::Machine &m) {
    m.cpu.pc=0x100;
    for(int i=0;i<3;++i)require(f3_dispatch(&m.cpu),"Synthetic unit span dispatches");
}
struct SynthSnapshot {
    std::array<uint8_t,0x20000> ram;std::array<uint8_t,0x40000> graphics;std::array<uint8_t,0x8000> palette;
    std::array<uint32_t,8> d,a;uint32_t pc;uint16_t sr;uint64_t cycles,native_blocks;uint32_t crc,sync_crc;
    explicit SynthSnapshot(f3rt::Machine &m):ram(m.ram),graphics(m.graphics),palette(m.palette),pc(m.cpu.pc),sr(m.cpu.sr),
        cycles(m.cpu.cycles),native_blocks(m.native_blocks),crc(m.state_crc()),sync_crc(m.sync_state_crc()) {
        std::copy(std::begin(m.cpu.d),std::end(m.cpu.d),d.begin());std::copy(std::begin(m.cpu.a),std::end(m.cpu.a),a.begin());
    }
    bool operator==(const SynthSnapshot &o) const {
        return ram==o.ram && graphics==o.graphics && palette==o.palette && d==o.d && a==o.a && pc==o.pc && sr==o.sr &&
               cycles==o.cycles && native_blocks==o.native_blocks && crc==o.crc && sync_crc==o.sync_crc;
    }
};
void check_sprite_unit_sandbox() {
    require(std::string(synth_bump_behaviour.name)=="synth-bump-behaviour","Behaviour names replace underscores with hyphens");
    {   // A completed replay leaves every piece of machine state untouched.
        auto m=synth_machine(true,true,false);
        const SynthSnapshot before(*m);
        m->cpu.pc=0x100;
        const SynthSnapshot at_unit(*m);
        f3_unit_enter(&m->cpu,0);
        require(SynthSnapshot(*m)==at_unit,"Sandbox replay leaves RAM, graphics, cycles, native_blocks and CPU untouched");
        const auto report=m->sprite_units->report();
        require(report.replays==1 && report.completed==1 && report.aborted==0,"Unit replay runs to its end PC");
        (void)before;
    }
    {   // Device access aborts the replay without touching the device.
        auto m=synth_machine(true,true,false);
        m->cpu.pc=0x200;
        const SynthSnapshot at_unit(*m);
        f3_unit_enter(&m->cpu,1);
        require(SynthSnapshot(*m)==at_unit,"Aborted replay leaves machine state untouched");
        const auto report=m->sprite_units->report();
        require(report.replays==1 && report.completed==0 && report.aborted==1 &&
                report.units[1].aborts[size_t(f3rt::SpriteUnits::Abort::Device)]==1,
                "I/O read aborts the replay");
    }
    {   // Unpatched replay matches the real span bit-exactly; the feature never changes machine state.
        auto on=synth_machine(true,true,false),off=synth_machine(false,false,false);
        synth_span(*on);synth_span(*off);
        require(SynthSnapshot(*on)==SynthSnapshot(*off),"Replay on/off yields identical state, cycles and native_blocks");
        require(on->ram[0x102]==0x12 && on->ram[0x103]==0x35,"Real span still executes against unpatched RAM");
        const auto report=on->sprite_units->report();
        require(report.matched==1 && report.mismatched==0 && report.aborted==0 && report.stray_writes==0 &&
                on->sprite_units->passed(),"Check mode matches the real written entries");
        const auto presented=on->sprite_units->presentation();
        require(presented.identity.size()==0x800 && presented.splices.empty(),"No behaviour means no splice");
        require(presented.identity[3] && presented.identity[4] && presented.identity[3]!=presented.identity[4] &&
                !presented.identity[2] && !presented.identity[5],"Written sprite entries receive per-entry identities");
        on->write16(0x600030,0x0001);
        require(!on->sprite_units->presentation().identity[3] && on->sprite_units->presentation().identity[4],
                "A write outside any unit clears that slot's identity");
        require(on->sprite_units->report().stray_unaccounted==2,"Check mode records unaccounted writers");
    }
    {   // A behaviour patches the replay's overlay only and the result is spliced over the real entries.
        auto m=synth_machine(true,false,true);
        synth_span(*m);
        require(m->ram[0x102]==0x12 && m->ram[0x103]==0x35 && m->graphics[0x32]==0x12 && m->graphics[0x33]==0x34,
                "Patched replay never reaches real RAM or sprite RAM");
        const auto presented=m->sprite_units->presentation();
        require(presented.splices.size()==1,"Behaviour produces one splice");
        const auto &splice=presented.splices[0];
        require(!splice.bank && splice.first==3 && splice.last==4 && splice.real.size()==32 && splice.replacement.size()==32 &&
                std::equal(splice.real.begin(),splice.real.end(),m->graphics.begin()+0x30) &&
                splice.replacement[0]==0xab && splice.replacement[1]==0xcd &&
                splice.replacement[2]==0x12 && splice.replacement[3]==0x39 &&
                splice.replacement[16]==0x11 && splice.identity,
                "Splice holds the real range and the patched replay's entries");
        require(m->sprite_units->report().spliced==1,"Splice is counted");
    }
}
}
int main() {
    return f3test::run("sprite emit-unit sandbox",[] { check_sprite_unit_sandbox(); });
}
