#[path = "../tests/support/mod.rs"]
mod support;

fn main() -> std::io::Result<()> {
    let path = std::env::args_os().nth(1).expect("output fixture directory is required");
    let root = std::path::PathBuf::from(path);
    std::fs::create_dir_all(&root)?;
    for (name, update, corrupt_file_data) in [
        ("driveclub-base-test.pkg", false, false),
        ("driveclub-update-test.pkg", true, false),
        ("driveclub-corrupt-test.pkg", false, true),
    ] {
        let path = root.join(name);
        if path.is_file() { std::fs::remove_file(&path)?; }
        support::write_fixture(&path, support::Options { update, corrupt_file_data, ..Default::default() })?;
        println!("{} ({} bytes)", path.display(), std::fs::metadata(&path)?.len());
    }
    Ok(())
}
