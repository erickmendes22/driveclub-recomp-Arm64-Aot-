//! JNI-facing, transactional PS4 FPKG extraction using the pinned Orbis parsers.
//! Input is a borrowed SAF descriptor. No package copy or executable subprocess is used.
use memmap2::MmapOptions;
use orbis_pfs::{directory::{DirEntry, Directory}, image::Image, pfsc::PfscImage};
use orbis_pkg::{entry::{EntryId, PkgEntry}, header::PkgHeader, Pkg};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, HashSet};
use std::ffi::{CStr, CString, c_char, c_void};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::path::{Component, Path, PathBuf};

type Result<T> = std::result::Result<T, String>;
const MAX_PACKAGE: u64 = 512 * 1024 * 1024 * 1024;
const MAX_OUTPUT: u64 = 256 * 1024 * 1024 * 1024;
const MAX_METADATA: usize = 64 * 1024 * 1024;
const MAX_FILES: usize = 200_000;
const CANCELLED: &str = "__CANCELLED__";

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Plan {
    pub title_id: String,
    pub title: String,
    pub version: String,
    pub content_id: String,
    pub fingerprint: String,
    pub is_update: bool,
    pub package_bytes: u64,
    pub extracted_bytes: u64,
}

fn error(context: &str, value: impl std::fmt::Debug) -> String {
    format!("{context}: {value:?}")
}
fn check(condition: bool, message: &str) -> Result<()> {
    if condition { Ok(()) } else { Err(message.to_owned()) }
}
fn io_error(context: &str, e: io::Error) -> String {
    if e.raw_os_error() == Some(libc::ENOSPC) {
        "Espaço insuficiente para extrair o PKG. A instalação anterior foi preservada.".into()
    } else { format!("{context}: {e}") }
}

fn safe_path(path: &Path) -> Result<()> {
    let text = path.to_str().ok_or("O PKG contém um nome de arquivo UTF-8 inválido.")?;
    check(!text.is_empty() && text.len() <= 1024 && !text.contains(['\\', '\0']), "Caminho inválido no PKG.")?;
    for component in path.components() {
        match component {
            Component::Normal(name) => {
                let name = name.to_str().ok_or("Nome de arquivo inválido no PKG.")?;
                check(name != "." && name != ".." && name.len() <= 255,
                      "O PKG tenta escrever fora da pasta de instalação.")?;
            }
            _ => return Err("O PKG tenta escrever fora da pasta de instalação.".into()),
        }
    }
    Ok(())
}

fn sfo_fields(bytes: &[u8]) -> Result<BTreeMap<String, String>> {
    let u16_at = |at: usize| -> Result<u16> {
        Ok(u16::from_le_bytes(bytes.get(at..at + 2).ok_or("PARAM.SFO truncado.")?.try_into().unwrap()))
    };
    let u32_at = |at: usize| -> Result<u32> {
        Ok(u32::from_le_bytes(bytes.get(at..at + 4).ok_or("PARAM.SFO truncado.")?.try_into().unwrap()))
    };
    check(bytes.len() >= 20 && bytes.len() <= 8 * 1024 * 1024 && u32_at(0)? == 0x46535000,
          "PARAM.SFO inválido no PKG.")?;
    let keys = u32_at(8)? as usize;
    let values = u32_at(12)? as usize;
    let count = u32_at(16)? as usize;
    check(count <= 4096 && 20 + count * 16 <= keys && keys < values && values <= bytes.len(),
          "Tabelas inválidas no PARAM.SFO.")?;
    let mut fields = BTreeMap::new();
    for n in 0..count {
        let entry = 20 + 16 * n;
        let key = keys + u16_at(entry)? as usize;
        let format = u16_at(entry + 2)?;
        let length = u32_at(entry + 4)? as usize;
        let maximum = u32_at(entry + 8)? as usize;
        let offset = values + u32_at(entry + 12)? as usize;
        check(key < values && length <= maximum && offset >= values,
              "Entrada inválida no PARAM.SFO.")?;
        let name = &bytes[key..values];
        let end = name.iter().position(|&b| b == 0).ok_or("Nome sem terminador no PARAM.SFO.")?;
        check(end > 0 && end <= 128, "Chave excessiva ou vazia no PARAM.SFO.")?;
        let name = std::str::from_utf8(&name[..end]).map_err(|_| "Chave inválida no PARAM.SFO.")?;
        let data = bytes.get(offset..offset + length).ok_or("Valor truncado no PARAM.SFO.")?;
        if format == 0x0204 {
            check(data.len() <= 4096, "Campo de texto excessivo no PARAM.SFO.")?;
            let end = data.iter().position(|&b| b == 0).unwrap_or(data.len());
            let value = std::str::from_utf8(&data[..end]).map_err(|_| "Texto inválido no PARAM.SFO.")?;
            check(fields.insert(name.to_owned(), value.to_owned()).is_none(), "Chave repetida no PARAM.SFO.")?;
        }
    }
    Ok(fields)
}

fn checked_header(bytes: &[u8]) -> Result<PkgHeader> {
    let header = PkgHeader::read(bytes).map_err(|e| error("Este arquivo não é um PKG PS4 válido", e))?;
    check(header.pkg_size() == bytes.len() as u64, "PKG incompleto, dividido ou com tamanho declarado inválido.")?;
    check(header.content_id().title_id() == "CUSA00093", "Este PKG não é Driveclub CUSA00093.")?;
    check(matches!(header.content_type(), 1 | 4 | 5 | 6 | 0x1a), "Este PKG não contém o jogo base ou uma atualização.")?;
    let flags = header.raw_header().pkg_content_flags.get();
    check(flags & 0x01000000 == 0 && !(header.content_type() == 4 && flags & 0x20000000 == 0),
          "Patch delta não suportado: depende de blocos de outra versão. Use uma atualização completa/cumulativa extraível.")?;
    check(header.raw_header().pfs_image_count.get() == 1,
          "PKG com múltiplas imagens ou partes PFS não suportado. Selecione um PKG completo.")?;
    check(header.entry_count() > 0 && header.entry_count() <= 65536, "Quantidade inválida de entradas no PKG.")?;
    let start = header.table_offset();
    let length = header.entry_count() * PkgEntry::RAW_SIZE;
    let table = bytes.get(start..start + length).ok_or("Tabela de entradas truncada no PKG.")?;
    let mut ids = HashSet::new();
    for raw in table.chunks_exact(PkgEntry::RAW_SIZE) {
        let entry = PkgEntry::read(raw).map_err(|e| error("Entrada PKG inválida", e))?;
        check(ids.insert(entry.id()), "Entrada duplicada no PKG.")?;
        if entry.id() == 0x10 { check(entry.data_size() == 2048, "Tabela de chaves PKG inválida.")?; }
        if entry.id() == 0x20 { check(entry.data_size() == 256, "Chave de imagem PKG inválida.")?; }
        let padded = (entry.data_size() + 15) & !15;
        check(entry.data_size() <= MAX_METADATA && bytes.get(entry.data_offset()..entry.data_offset() + padded).is_some(),
              "Entrada PKG truncada ou com tamanho excessivo.")?;
    }
    check(header.pfs_size() > 0 && bytes.get(header.pfs_offset()..header.pfs_offset() + header.pfs_size()).is_some(),
          "Imagem PFS ausente ou truncada no PKG.")?;
    Ok(header)
}

struct Work<'a> { path: PathBuf, image: Box<dyn Image + 'a> }
struct Contents<'a> {
    files: Vec<Work<'a>>,
    directories: Vec<PathBuf>,
    metadata: Vec<(PathBuf, Vec<u8>)>,
    total: u64,
}

fn directory_root<'a>(pfs: &std::sync::Arc<orbis_pfs::Pfs<'a>>) -> Result<Directory<'a>> {
    let mut root = pfs.root().open().map_err(|e| error("Não foi possível abrir a raiz PFS", e))?;
    match root.remove(b"uroot") {
        Some(DirEntry::Directory(dir)) => Ok(dir),
        _ => Err("A imagem PFS não contém a pasta uroot esperada.".into()),
    }
}

fn collect<'a>(dir: Directory<'a>, parent: &Path, depth: usize, names: &mut HashSet<String>, output: &mut Contents<'a>) -> Result<()> {
    check(depth <= 64, "A árvore de diretórios do PKG é excessivamente profunda ou cíclica.")?;
    check(dir.mode() & 0xf000 == 0x4000, "Tipo de diretório PFS inválido.")?;
    for (name, item) in dir.open().map_err(|e| error("Diretório PFS inválido", e))? {
        let name = std::str::from_utf8(&name).map_err(|_| "Nome PFS não é UTF-8 válido.")?;
        check(!name.is_empty() && !name.contains(['/', '\\', '\0']) && name != "." && name != "..", "Nome inseguro no PFS.")?;
        let path = parent.join(name);
        safe_path(&path)?;
        check(names.len() < MAX_FILES && names.insert(path.to_string_lossy().to_ascii_lowercase()), "PKG contém caminhos repetidos ou arquivos demais.")?;
        match item {
            DirEntry::Directory(next) => {
                output.directories.push(path.clone());
                collect(next, &path, depth + 1, names, output)?;
            }
            DirEntry::File(file) => {
                check(file.mode() & 0xf000 == 0x8000, "Links ou tipos especiais de arquivo não são aceitos no PKG.")?;
                let compressed = file.is_compressed();
                let image: Box<dyn Image + 'a> = if compressed {
                    Box::new(PfscImage::open(file.into_image()).map_err(|e| error("Arquivo PFSC inválido", e))?)
                } else { Box::new(file.into_image()) };
                output.total = output.total.checked_add(image.len()).ok_or("Tamanho de extração inválido.")?;
                check(output.total <= MAX_OUTPUT, "O conteúdo expandido excede o limite de 256 GiB deste instalador.")?;
                output.files.push(Work { path, image });
            }
            _ => return Err("Tipo de entrada PFS não suportado.".into()),
        }
    }
    Ok(())
}

fn contents<'a>(pkg: &'a Pkg<&'a [u8]>) -> Result<Contents<'a>> {
    let mut output = Contents { files: Vec::new(), directories: Vec::new(), metadata: Vec::new(), total: 0 };
    let image = pkg.get_pfs_image().ok_or("Imagem PFS truncada.")?;
    let outer = orbis_pfs::open_slice(image.data, Some(image.ekpfs)).map_err(|e| error("PFS externo inválido ou criptografia não suportada", e))?;
    let mut root = directory_root(&outer)?.open().map_err(|e| error("PFS externo inválido", e))?;
    let inner = match root.remove(b"pfs_image.dat") {
        Some(DirEntry::File(file)) => file,
        _ => return Err("PKG sem pfs_image.dat completo; imagens delta/divididas não são suportadas.".into()),
    };
    let compressed = inner.is_compressed();
    let inner = if compressed {
        let pfsc = PfscImage::open(inner.into_image()).map_err(|e| error("PFSC interno inválido", e))?;
        orbis_pfs::open_image(pfsc)
    } else { orbis_pfs::open_image(inner.into_image()) }
        .map_err(|e| error("PFS interno inválido; não é possível resolver blocos externos à atualização", e))?;
    let mut names = HashSet::new();
    collect(directory_root(&inner)?, Path::new(""), 0, &mut names, &mut output)?;
    check(output.files.iter().any(|w| w.path == Path::new("eboot.bin")), "O PKG não contém eboot.bin completo.")?;
    let mut metadata_bytes = 0usize;
    for value in pkg.entries() {
        let (_, entry) = value.map_err(|e| error("Entrada PKG inválida", e))?;
        let Some(path) = entry.to_path("sce_sys") else { continue };
        safe_path(&path)?;
        check(names.insert(path.to_string_lossy().to_ascii_lowercase()), "Metadados e PFS contêm caminhos em conflito.")?;
        let data = pkg.entry_data(&entry).map_err(|e| error("Não foi possível decifrar uma entrada PKG; apenas FPKGs compatíveis são aceitos", e))?;
        metadata_bytes = metadata_bytes.checked_add(data.len()).ok_or("Metadados PKG excessivos.")?;
        check(metadata_bytes <= MAX_METADATA, "Metadados PKG excedem o limite de 64 MiB.")?;
        output.total += data.len() as u64;
        check(output.total <= MAX_OUTPUT, "O conteúdo expandido excede o limite deste instalador.")?;
        output.metadata.push((path, data));
    }
    Ok(output)
}

fn prepare<'a>(pkg: &'a Pkg<&'a [u8]>, raw: &[u8], modified: (i64, i64)) -> Result<(Plan, Contents<'a>)> {
    let (entry, _) = pkg.find_entry(EntryId::ParamSfo).map_err(|e| error("PKG sem PARAM.SFO", e))?;
    let sfo = pkg.entry_data(&entry).map_err(|e| error("Não foi possível ler PARAM.SFO", e))?;
    let fields = sfo_fields(&sfo)?;
    check(fields.get("TITLE_ID").map(String::as_str) == Some("CUSA00093"), "PARAM.SFO não é Driveclub CUSA00093.")?;
    let category = fields.get("CATEGORY").map(String::as_str).unwrap_or("");
    check(matches!(category, "gd" | "gp"), "O PKG não é um jogo base ou uma atualização PS4.")?;
    let flags = pkg.header().raw_header().pkg_content_flags.get();
    let is_update = category == "gp" || flags & (0x00100000 | 0x00200000 | 0x40000000 | 0x20000000) != 0;
    let version = fields.get("APP_VER").or_else(|| fields.get("VERSION")).ok_or("PKG sem versão do aplicativo.")?.clone();
    check(!version.is_empty() && version.len() <= 16, "Versão inválida no PKG.")?;
    let output = contents(pkg)?;
    if !is_update {
        check(output.files.iter().any(|w| w.path == Path::new("game000.dat")), "O PKG base não contém game000.dat.")?;
    }
    let mut hash = Sha256::new();
    hash.update(&raw[..0x1000]);
    hash.update(&raw[pkg.header().table_offset()..pkg.header().table_offset() + pkg.entry_count() * PkgEntry::RAW_SIZE]);
    hash.update(&sfo);
    hash.update((raw.len() as u64).to_le_bytes());
    hash.update(modified.0.to_le_bytes());
    hash.update(modified.1.to_le_bytes());
    let plan = Plan {
        title_id: "CUSA00093".into(), title: fields.get("TITLE").cloned().unwrap_or_else(|| "DRIVECLUB".into()),
        version, content_id: pkg.header().content_id().as_str().into(), fingerprint: format!("{:x}", hash.finalize()),
        is_update, package_bytes: raw.len() as u64, extracted_bytes: output.total,
    };
    Ok((plan, output))
}

struct StagingGuard { root: PathBuf, keep: bool }
impl Drop for StagingGuard {
    fn drop(&mut self) {
        if self.keep { return; }
        if let Ok(entries) = fs::read_dir(&self.root) {
            for entry in entries.flatten() {
                if entry.file_type().map(|ty| ty.is_dir()).unwrap_or(false) { let _ = fs::remove_dir_all(entry.path()); }
                else { let _ = fs::remove_file(entry.path()); }
            }
        }
    }
}

fn extract_contents(output: &Contents<'_>, destination: &Path, progress: &mut dyn FnMut(u64, u64, &str) -> bool) -> Result<()> {
    check(destination.is_absolute(), "O destino PKG precisa ser uma pasta privada absoluta.")?;
    let info = fs::symlink_metadata(destination).map_err(|e| io_error("Não foi possível abrir staging", e))?;
    check(info.is_dir() && !info.file_type().is_symlink(), "O staging não é uma pasta válida.")?;
    check(fs::read_dir(destination).map_err(|e| io_error("Não foi possível listar staging", e))?.next().is_none(), "O instalador só escreve em uma nova pasta vazia.")?;
    let mut guard = StagingGuard { root: destination.to_owned(), keep: false };
    let mut completed = 0u64;
    let mut buffer = vec![0u8; 1024 * 1024];
    for directory in &output.directories {
        fs::create_dir_all(destination.join(directory)).map_err(|e| io_error("Não foi possível criar pasta", e))?;
    }
    for (relative, data) in &output.metadata {
        if !progress(completed, output.total, &relative.to_string_lossy()) { return Err(CANCELLED.into()); }
        let path = destination.join(relative);
        fs::create_dir_all(path.parent().unwrap()).map_err(|e| io_error("Não foi possível criar sce_sys", e))?;
        let mut file = OpenOptions::new().write(true).create_new(true).mode(0o600).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path).map_err(|e| io_error("Não foi possível criar metadados", e))?;
        file.write_all(data).and_then(|_| file.sync_all()).map_err(|e| io_error("Falha ao gravar metadados", e))?;
        completed += data.len() as u64;
    }
    for work in &output.files {
        let name = work.path.to_string_lossy();
        if !progress(completed, output.total, &name) { return Err(CANCELLED.into()); }
        let path = destination.join(&work.path);
        fs::create_dir_all(path.parent().unwrap()).map_err(|e| io_error("Não foi possível criar pasta", e))?;
        let mut file = OpenOptions::new().write(true).create_new(true).mode(0o600).custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path).map_err(|e| io_error("Não foi possível criar arquivo", e))?;
        let mut offset = 0;
        while offset < work.image.len() {
            if !progress(completed, output.total, &name) { return Err(CANCELLED.into()); }
            let n = work.image.read_at(offset, &mut buffer).map_err(|e| io_error("Erro ao ler/descomprimir PFS", e))?;
            check(n > 0 && n as u64 <= work.image.len() - offset, "Arquivo PFS truncado.")?;
            file.write_all(&buffer[..n]).map_err(|e| io_error("Erro ao gravar arquivo", e))?;
            completed += n as u64;
            offset += n as u64;
        }
        file.sync_all().map_err(|e| io_error("Falha ao sincronizar arquivo", e))?;
    }
    File::open(destination).and_then(|f| f.sync_all()).map_err(|e| io_error("Falha ao sincronizar staging", e))?;
    if !progress(completed, output.total, "Concluído") { return Err(CANCELLED.into()); }
    guard.keep = true;
    Ok(())
}

fn operation(fd: i32, destination: Option<(&Path, &str)>, progress: &mut dyn FnMut(u64, u64, &str) -> bool) -> Result<Plan> {
    let copied = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, 0) };
    if copied < 0 { return Err(io_error("Descritor PKG inválido", io::Error::last_os_error())); }
    let source = unsafe { File::from_raw_fd(copied) };
    let before = source.metadata().map_err(|e| io_error("Não foi possível consultar o PKG", e))?;
    check(before.is_file() && (0x1000..=MAX_PACKAGE).contains(&before.len()),
          "Selecione um PKG completo em armazenamento local; o provedor precisa fornecer um arquivo com acesso direto.")?;
    check(unsafe { libc::lseek(source.as_raw_fd(), 0, libc::SEEK_CUR) } >= 0, "O provedor não permite acesso aleatório ao PKG. Use um arquivo local.")?;
    // Mapping is read-only and demand-paged; a multi-gigabyte PKG is not copied into RAM or storage.
    let bytes = unsafe { MmapOptions::new().map(&source) }.map_err(|e| io_error("O provedor não permite mapear o PKG local", e))?;
    let _validated_header = checked_header(&bytes)?;
    let pkg = Pkg::new(&bytes[..]).map_err(|e| error("PKG criptografado não suportado ou corrompido. Use um FPKG extraível do seu dump; chaves retail não são incluídas", e))?;
    let (plan, output) = prepare(&pkg, &bytes, (before.mtime(), before.mtime_nsec()))?;
    if let Some((root, fingerprint)) = destination {
        check(fingerprint == plan.fingerprint, "O PKG mudou desde a inspeção. Selecione-o novamente.")?;
        extract_contents(&output, root, progress)?;
        let after = source.metadata().map_err(|e| io_error("Não foi possível revalidar o PKG", e))?;
        if before.len() != after.len() || before.mtime() != after.mtime() || before.mtime_nsec() != after.mtime_nsec() {
            // This directory is still unpublished; discard only the newly extracted generation.
            let _cleanup = StagingGuard { root: root.to_owned(), keep: false };
            return Err("O arquivo PKG foi alterado durante a extração.".into());
        }
    }
    Ok(plan)
}

fn response(result: Result<Plan>) -> *mut c_char {
    let json = match result {
        Ok(plan) => serde_json::json!({"ok": true, "plan": plan}),
        Err(message) => serde_json::json!({"ok": false, "cancelled": message == CANCELLED, "error": message}),
    };
    CString::new(json.to_string()).unwrap().into_raw()
}

#[unsafe(no_mangle)]
pub extern "C" fn driveclub_pkg_inspect(fd: i32) -> *mut c_char {
    response(std::panic::catch_unwind(|| operation(fd, None, &mut |_, _, _| true))
        .unwrap_or_else(|_| Err("PKG inválido: o parser interrompeu a leitura antes da instalação.".into())))
}

pub type Progress = unsafe extern "C" fn(*mut c_void, u64, u64, *const c_char) -> bool;
#[unsafe(no_mangle)]
pub unsafe extern "C" fn driveclub_pkg_extract(fd: i32, root: *const c_char, fingerprint: *const c_char,
        opaque: *mut c_void, callback: Option<Progress>) -> *mut c_char {
    response(std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        check(!root.is_null() && !fingerprint.is_null(), "Argumentos inválidos para instalação PKG.")?;
        let root = unsafe { CStr::from_ptr(root) }.to_str().map_err(|_| "Destino PKG inválido.")?;
        let fingerprint = unsafe { CStr::from_ptr(fingerprint) }.to_str().map_err(|_| "Identificador PKG inválido.")?;
        operation(fd, Some((Path::new(root), fingerprint)), &mut |done, total, name| {
            callback.map(|f| {
                let name = CString::new(name).unwrap();
                unsafe { f(opaque, done, total, name.as_ptr()) }
            }).unwrap_or(true)
        })
    })).unwrap_or_else(|_| Err("PKG inválido: a extração foi interrompida; a instalação anterior foi preservada.".into())))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn driveclub_pkg_free(value: *mut c_char) {
    if !value.is_null() { drop(unsafe { CString::from_raw(value) }); }
}

#[cfg(test)]
mod tests;
