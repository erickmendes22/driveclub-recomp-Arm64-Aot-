use super::*;
use std::os::unix::fs::FileExt;
use std::sync::atomic::{AtomicU64, Ordering};
#[path = "../tests/support/mod.rs"]
mod support;
use support::{Options, write_fixture};

struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let root = std::env::temp_dir().join(format!("driveclub-pkg-test-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
        fs::create_dir(&root).unwrap();
        Self(root)
    }
    fn stage(&self, name: &str) -> PathBuf { let path = self.0.join(name); fs::create_dir(&path).unwrap(); path }
}
impl Drop for Temp { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }
fn inspect(file: &File) -> Result<Plan> { operation(file.as_raw_fd(), None, &mut |_, _, _| true) }
fn install(file: &File, plan: &Plan, root: &Path) -> Result<Plan> {
    operation(file.as_raw_fd(), Some((root, &plan.fingerprint)), &mut |_, _, _| true)
}

#[test]
fn encrypted_signed_compressed_pkg_round_trip() {
    let temp = Temp::new(); let source = write_fixture(&temp.0.join("base.pkg"), Options::default()).unwrap();
    let encoded = driveclub_pkg_inspect(source.as_raw_fd());
    let json: serde_json::Value = serde_json::from_str(unsafe { CStr::from_ptr(encoded) }.to_str().unwrap()).unwrap();
    unsafe { driveclub_pkg_free(encoded); }
    assert_eq!(json["ok"], true); assert_eq!(json["plan"]["titleId"], "CUSA00093");
    let plan = inspect(&source).unwrap(); assert!(!plan.is_update); assert_eq!(plan.version, "01.00");
    let stage = temp.stage("stage"); let result = install(&source, &plan, &stage).unwrap();
    assert_eq!(result.fingerprint, plan.fingerprint);
    assert_eq!(fs::read(stage.join("eboot.bin")).unwrap(), support::BASE_EBOOT);
    assert_eq!(fs::read(stage.join("game000.dat")).unwrap(), support::BASE_ARCHIVE);
    assert_eq!(sfo_fields(&fs::read(stage.join("sce_sys/param.sfo")).unwrap()).unwrap()["TITLE_ID"], "CUSA00093");
}

#[test]
fn update_is_a_separate_overlay_and_existing_destination_is_never_overwritten() {
    let temp = Temp::new(); let base = temp.stage("existing-base");
    fs::write(base.join("game000.dat"), b"keep-existing-archive").unwrap();
    let source = write_fixture(&temp.0.join("update.pkg"), Options { update: true, ..Default::default() }).unwrap();
    let plan = inspect(&source).unwrap(); assert!(plan.is_update); assert_eq!(plan.version, "01.28");
    assert!(install(&source, &plan, &base).unwrap_err().contains("vazia"));
    assert_eq!(fs::read(base.join("game000.dat")).unwrap(), b"keep-existing-archive");
    let stage = temp.stage("update-stage"); install(&source, &plan, &stage).unwrap();
    assert!(!stage.join("game000.dat").exists());
    assert_eq!(fs::read(stage.join("eboot.bin")).unwrap(), support::UPDATE_EBOOT);
    assert_eq!(fs::read(stage.join("update-only.bin")).unwrap(), support::UPDATE_DATA);
    assert_eq!(fs::read(base.join("game000.dat")).unwrap(), b"keep-existing-archive");
}

#[test]
fn wrong_title_unsupported_encryption_delta_and_truncation_fail_before_writes() {
    let temp = Temp::new();
    let wrong = write_fixture(&temp.0.join("wrong.pkg"), Options { title: "CUSA00094", ..Default::default() }).unwrap();
    assert!(inspect(&wrong).unwrap_err().contains("CUSA00093"));
    let encrypted = write_fixture(&temp.0.join("encrypted.pkg"), Options::default()).unwrap();
    encrypted.write_all_at(&[0; 256], 0x2800).unwrap();
    assert!(inspect(&encrypted).unwrap_err().contains("criptografado"));
    let delta = write_fixture(&temp.0.join("delta.pkg"), Options { update: true, ..Default::default() }).unwrap();
    delta.write_all_at(&0x41000000u32.to_be_bytes(), 0x78).unwrap();
    assert!(inspect(&delta).unwrap_err().contains("delta"));
    let truncated = write_fixture(&temp.0.join("truncated.pkg"), Options::default()).unwrap();
    truncated.set_len(truncated.metadata().unwrap().len() - 1).unwrap();
    assert!(inspect(&truncated).unwrap_err().contains("tamanho"));
}

#[test]
fn traversal_and_unbounded_pfs_allocations_are_rejected() {
    let temp = Temp::new();
    let traversal = write_fixture(&temp.0.join("traversal.pkg"), Options { file_name: Some("../escaped"), ..Default::default() }).unwrap();
    assert!(inspect(&traversal).unwrap_err().contains("inseguro"));
    assert!(!temp.0.parent().unwrap().join("escaped").exists());
    let invalid = write_fixture(&temp.0.join("invalid.pkg"), Options { invalid_pfs_geometry: true, ..Default::default() }).unwrap();
    assert!(inspect(&invalid).unwrap_err().contains("InvalidGeometry"));
}

#[test]
fn cancellation_and_corrupt_file_roll_back_new_staging_only() {
    let temp = Temp::new(); fs::write(temp.0.join("existing-save"), b"save").unwrap();
    let source = write_fixture(&temp.0.join("large.pkg"), Options { executable_size: Some(3 * 1024 * 1024), ..Default::default() }).unwrap();
    let plan = inspect(&source).unwrap(); let stage = temp.stage("cancelled");
    let result = operation(source.as_raw_fd(), Some((&stage, &plan.fingerprint)), &mut |done, _, _| done < 1024 * 1024);
    assert_eq!(result.unwrap_err(), CANCELLED); assert_eq!(fs::read_dir(&stage).unwrap().count(), 0);
    let corrupt = write_fixture(&temp.0.join("corrupt.pkg"), Options { corrupt_file_data: true, ..Default::default() }).unwrap();
    let plan = inspect(&corrupt).unwrap(); let stage = temp.stage("corrupt-stage");
    assert!(install(&corrupt, &plan, &stage).is_err()); assert_eq!(fs::read_dir(stage).unwrap().count(), 0);
    assert_eq!(fs::read(temp.0.join("existing-save")).unwrap(), b"save");
}

#[test]
fn offsets_beyond_four_gib_are_read_without_materializing_the_package() {
    let temp = Temp::new();
    let source = write_fixture(&temp.0.join("sparse.pkg"), Options { pfs_offset: 0x1_0001_0000, signed: false, ..Default::default() }).unwrap();
    let plan = inspect(&source).unwrap(); assert!(plan.package_bytes > 4 * 1024 * 1024 * 1024);
    let stage = temp.stage("large-offset"); install(&source, &plan, &stage).unwrap();
    assert_eq!(fs::read(stage.join("game000.dat")).unwrap(), support::BASE_ARCHIVE);
}

#[test]
fn changed_selection_fingerprint_is_rejected_before_extraction() {
    let temp = Temp::new(); let source = write_fixture(&temp.0.join("base.pkg"), Options::default()).unwrap();
    let mut plan = inspect(&source).unwrap(); plan.fingerprint = "wrong".into();
    let stage = temp.stage("stage"); assert!(install(&source, &plan, &stage).unwrap_err().contains("mudou"));
    assert_eq!(fs::read_dir(stage).unwrap().count(), 0);
}
