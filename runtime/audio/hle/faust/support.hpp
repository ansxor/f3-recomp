// Name-to-zone registry for FAUST-generated classes (generated/*.hpp, built with architecture.hpp).
// The FAUST interfaces (dsp, UI, Meta) are the unmodified headers in runtime/third_party/faust.
#pragma once

#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace f3rt::hle::faust {

// Name-to-zone registry for one instance. Zones point into the instance, so they are valid only
// while that instance lives at its current address.
class Controls final : private UI {
public:
    explicit Controls(dsp& instance) { instance.buildUserInterface(this); }

    // Throws std::logic_error when no widget has this label.
    FAUSTFLOAT* zone(std::string_view label) const {
        const auto it = zones_.find(label);
        if (it == zones_.end()) {
            throw std::logic_error("faust: no control labelled '" + std::string(label) + "'");
        }
        return it->second;
    }

    void set(std::string_view label, FAUSTFLOAT value) const { *zone(label) = value; }

private:
    void add(const char* label, FAUSTFLOAT* zone) {
        if (!zones_.try_emplace(label, zone).second) {
            throw std::logic_error(std::string("faust: duplicate control labelled '") + label + "'");
        }
    }

    void openTabBox(const char*) override {}
    void openHorizontalBox(const char*) override {}
    void openVerticalBox(const char*) override {}
    void closeBox() override {}

    void addButton(const char* label, FAUSTFLOAT* zone) override { add(label, zone); }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override { add(label, zone); }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                           FAUSTFLOAT) override {
        add(label, zone);
    }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                             FAUSTFLOAT) override {
        add(label, zone);
    }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT,
                     FAUSTFLOAT) override {
        add(label, zone);
    }
    void addHorizontalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT) override {
        add(label, zone);
    }
    void addVerticalBargraph(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT) override {
        add(label, zone);
    }
    void addSoundfile(const char* label, const char*, ::Soundfile**) override {
        throw std::logic_error(std::string("faust: soundfile control '") + label + "' is not supported");
    }

    std::map<std::string, FAUSTFLOAT*, std::less<>> zones_;
};

}  // namespace f3rt::hle::faust
