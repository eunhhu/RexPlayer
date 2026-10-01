use rex_ui::options::{HELP, UiOptions};
#[cfg(target_os = "linux")]
mod native;
fn main() {
    let options = match UiOptions::parse(std::env::args_os().skip(1)) {
        Ok(Some(options)) => options,
        Ok(None) => {
            println!("{HELP}");
            return;
        }
        Err(error) => {
            eprintln!("{error}\n{HELP}");
            std::process::exit(64);
        }
    };
    #[cfg(target_os = "linux")]
    native::run(options);
    #[cfg(not(target_os = "linux"))]
    {
        let _ = options;
        eprintln!("RexPlayer currently supports Linux x86_64/aarch64 only.");
        std::process::exit(69);
    }
}
