use std::env;

fn main() {
    println!("cargo:rustc-check-cfg=cfg(f3_video_shaders)");
    println!("cargo:rerun-if-env-changed=F3_VIDEO_SHADERS_RS");
    if env::var_os("F3_VIDEO_SHADERS_RS").is_some_and(|path| !path.is_empty()) {
        println!("cargo:rustc-cfg=f3_video_shaders");
    }
}
