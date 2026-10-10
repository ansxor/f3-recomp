#pragma once
#include "f3rt/machine.hpp"
#include <exception>
#include <iostream>
#include <stdexcept>

namespace f3test {
inline void require(bool ok,const char *why) { if(!ok)throw std::runtime_error(why); }
// Minimal ROM set: SSP 41fff0, PC 100, `moveq #42,d0; bra self`, one OTIS sample word at 0x12345.
f3rt::RomSet fixture();
// Runs one test binary's checks: prints "PASS <area>" and returns 0, or "FAIL <why>" and returns 1.
template<class Body> int run(const char *area,Body &&body) {
    try { body(); }
    catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n';return 1; }
    std::cout<<"PASS "<<area<<'\n';
    return 0;
}
}
