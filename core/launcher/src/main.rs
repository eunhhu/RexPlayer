use rex_launcher::{run, HostContext, Options, ProcessBackend};

fn main() {
    let options = match Options::parse(std::env::args_os().skip(1)) {
        Ok(Some(options)) => options,
        Ok(None) => {
            println!("{}", rex_launcher::HELP);
            return;
        }
        Err(error) => {
            eprintln!("{error}\nUse rex-launcher --help for usage.");
            std::process::exit(64);
        }
    };
    let mut backend = ProcessBackend;
    let report = run(&options, &HostContext::current(), &mut backend);
    if report.exit_code == 0 {
        println!("{}", report.message);
    } else {
        eprintln!("{}", report.message);
    }
    std::process::exit(report.exit_code);
}
