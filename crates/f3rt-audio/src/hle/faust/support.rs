//! FAUST interfaces and the name-to-parameter registry: the Rust counterparts of the unmodified FAUST
//! headers in `runtime/third_party/faust` (as the generated Rust modules declare them) and of
//! `runtime/audio/hle/faust/support.hpp`.
//!
//! The C++ interfaces hand the UI a `FAUSTFLOAT*` zone; the Rust backend hands it a `ParamIndex`
//! and reaches the same storage through `FaustDsp::get_param`/`FaustDsp::set_param`, so `Controls`
//! records indices instead of pointers and `Controls::set` takes the instance as well.

use std::collections::BTreeMap;

pub type F32 = f32;
pub type F64 = f64;
pub type FaustFloat = F32;

#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct ParamIndex(pub i32);

pub struct Soundfile<'a, T> {
    pub buffers: &'a &'a T,
    pub length: &'a i32,
    pub sample_rate: &'a i32,
    pub offset: &'a i32,
    pub channels: i32,
}

pub trait FaustDsp {
    type T;

    fn new() -> Self
    where
        Self: Sized;
    fn metadata(&self, m: &mut dyn Meta);
    fn get_sample_rate(&self) -> i32;
    fn get_num_inputs(&self) -> i32;
    fn get_num_outputs(&self) -> i32;
    fn class_init(sample_rate: i32)
    where
        Self: Sized;
    fn instance_reset_params(&mut self);
    fn instance_clear(&mut self);
    fn instance_constants(&mut self, sample_rate: i32);
    fn instance_init(&mut self, sample_rate: i32);
    fn init(&mut self, sample_rate: i32);
    fn build_user_interface(&self, ui_interface: &mut dyn UI<Self::T>);
    fn build_user_interface_static(ui_interface: &mut dyn UI<Self::T>)
    where
        Self: Sized;
    fn get_param(&self, param: ParamIndex) -> Option<Self::T>;
    fn set_param(&mut self, param: ParamIndex, value: Self::T);
    fn compute(&mut self, count: i32, inputs: &[&[Self::T]], outputs: &mut [&mut [Self::T]]);
}

pub trait Meta {
    fn declare(&mut self, key: &str, value: &str);
}

pub trait UI<T> {
    // -- widget's layouts
    fn open_tab_box(&mut self, label: &str);
    fn open_horizontal_box(&mut self, label: &str);
    fn open_vertical_box(&mut self, label: &str);
    fn close_box(&mut self);

    // -- active widgets
    fn add_button(&mut self, label: &str, param: ParamIndex);
    fn add_check_button(&mut self, label: &str, param: ParamIndex);
    fn add_vertical_slider(&mut self, label: &str, param: ParamIndex, init: T, min: T, max: T, step: T);
    fn add_horizontal_slider(
        &mut self,
        label: &str,
        param: ParamIndex,
        init: T,
        min: T,
        max: T,
        step: T,
    );
    fn add_num_entry(&mut self, label: &str, param: ParamIndex, init: T, min: T, max: T, step: T);

    // -- passive widgets
    fn add_horizontal_bargraph(&mut self, label: &str, param: ParamIndex, min: T, max: T);
    fn add_vertical_bargraph(&mut self, label: &str, param: ParamIndex, min: T, max: T);

    // -- metadata declarations
    fn declare(&mut self, param: Option<ParamIndex>, key: &str, value: &str);
}

/// Name-to-parameter registry for one instance (`faust::Controls` in support.hpp). The indices point
/// into the instance they were collected from, so they are only meaningful for that instance.
pub struct Controls {
    zones: BTreeMap<String, ParamIndex>,
}

impl Controls {
    pub fn new<D: FaustDsp<T = FaustFloat>>(instance: &D) -> Controls {
        let mut controls = Controls { zones: BTreeMap::new() };
        instance.build_user_interface(&mut controls);
        controls
    }

    /// Panics when no widget has this label.
    pub fn zone(&self, label: &str) -> ParamIndex {
        match self.zones.get(label) {
            Some(zone) => *zone,
            None => panic!("faust: no control labelled '{}'", label),
        }
    }

    pub fn set<D: FaustDsp<T = FaustFloat>>(&self, instance: &mut D, label: &str, value: FaustFloat) {
        let zone = self.zone(label);
        instance.set_param(zone, value);
    }

    fn add(&mut self, label: &str, zone: ParamIndex) {
        if self.zones.insert(label.to_owned(), zone).is_some() {
            panic!("faust: duplicate control labelled '{}'", label);
        }
    }
}

impl UI<FaustFloat> for Controls {
    fn open_tab_box(&mut self, _label: &str) {}
    fn open_horizontal_box(&mut self, _label: &str) {}
    fn open_vertical_box(&mut self, _label: &str) {}
    fn close_box(&mut self) {}

    fn add_button(&mut self, label: &str, zone: ParamIndex) {
        self.add(label, zone);
    }
    fn add_check_button(&mut self, label: &str, zone: ParamIndex) {
        self.add(label, zone);
    }
    fn add_vertical_slider(
        &mut self,
        label: &str,
        zone: ParamIndex,
        _init: FaustFloat,
        _min: FaustFloat,
        _max: FaustFloat,
        _step: FaustFloat,
    ) {
        self.add(label, zone);
    }
    fn add_horizontal_slider(
        &mut self,
        label: &str,
        zone: ParamIndex,
        _init: FaustFloat,
        _min: FaustFloat,
        _max: FaustFloat,
        _step: FaustFloat,
    ) {
        self.add(label, zone);
    }
    fn add_num_entry(
        &mut self,
        label: &str,
        zone: ParamIndex,
        _init: FaustFloat,
        _min: FaustFloat,
        _max: FaustFloat,
        _step: FaustFloat,
    ) {
        self.add(label, zone);
    }

    fn add_horizontal_bargraph(
        &mut self,
        label: &str,
        zone: ParamIndex,
        _min: FaustFloat,
        _max: FaustFloat,
    ) {
        self.add(label, zone);
    }
    fn add_vertical_bargraph(
        &mut self,
        label: &str,
        zone: ParamIndex,
        _min: FaustFloat,
        _max: FaustFloat,
    ) {
        self.add(label, zone);
    }

    fn declare(&mut self, _param: Option<ParamIndex>, _key: &str, _value: &str) {}
}
