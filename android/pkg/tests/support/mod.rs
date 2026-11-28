//! Copyright-free synthetic PKGs for exercising the real parser and crypto path.
//! No game executables or game assets are included or executed by these tests.
#![allow(dead_code)]
use aes::{Aes128, cipher::{BlockEncryptMut, KeyInit, KeyIvInit}};
use flate2::{Compression, write::ZlibEncoder};
use hmac::{Hmac, Mac};
use rand::{SeedableRng, rngs::StdRng};
use rsa::{Pkcs1v15Encrypt, RsaPublicKey};
use sha2::{Digest, Sha256};
use std::{fs::{File, OpenOptions}, io::{self, Write}, os::unix::fs::FileExt, path::Path};
use xts_mode::{Xts128, get_tweak_default};

pub const BLOCK: usize = 0x10000;
pub const BASE_EBOOT: &[u8] = b"BASE-EXECUTABLE-FIXTURE\n";
pub const BASE_ARCHIVE: &[u8] = b"BASE-ARCHIVE-FIXTURE\n";
pub const UPDATE_EBOOT: &[u8] = b"UPDATE-EXECUTABLE-FIXTURE\n";
pub const UPDATE_DATA: &[u8] = b"UPDATE-PAYLOAD\n";

fn le16(data: &mut [u8], at: usize, value: u16) { data[at..at + 2].copy_from_slice(&value.to_le_bytes()); }
fn le32(data: &mut [u8], at: usize, value: u32) { data[at..at + 4].copy_from_slice(&value.to_le_bytes()); }
fn le64(data: &mut [u8], at: usize, value: u64) { data[at..at + 8].copy_from_slice(&value.to_le_bytes()); }
fn be32(data: &mut [u8], at: usize, value: u32) { data[at..at + 4].copy_from_slice(&value.to_be_bytes()); }
fn be64(data: &mut [u8], at: usize, value: u64) { data[at..at + 8].copy_from_slice(&value.to_be_bytes()); }
fn aligned(n: usize, alignment: usize) -> usize { (n + alignment - 1) & !(alignment - 1) }

pub fn sfo(title: &str, update: bool) -> Vec<u8> {
    let fields = [
        ("TITLE_ID", title), ("TITLE", "DRIVECLUB test fixture"),
        ("APP_VER", if update { "01.28" } else { "01.00" }),
        ("CATEGORY", if update { "gp" } else { "gd" }),
    ];
    let keys = 20 + fields.len() * 16;
    let mut data = vec![0u8; keys];
    le32(&mut data, 0, 0x46535000); le32(&mut data, 4, 0x00000101);
    le32(&mut data, 8, keys as u32); le32(&mut data, 16, fields.len() as u32);
    for (n, (name, _)) in fields.iter().enumerate() {
        let offset = data.len() - keys;
        le16(&mut data, 20 + n * 16, offset as u16);
        data.extend_from_slice(name.as_bytes()); data.push(0);
    }
    data.resize(aligned(data.len(), 4), 0);
    let values = data.len(); le32(&mut data, 12, values as u32);
    for (n, (_, value)) in fields.iter().enumerate() {
        let entry = 20 + n * 16;
        let offset = data.len() - values;
        le16(&mut data, entry + 2, 0x0204);
        le32(&mut data, entry + 4, value.len() as u32 + 1);
        le32(&mut data, entry + 8, value.len() as u32 + 1);
        le32(&mut data, entry + 12, offset as u32);
        data.extend_from_slice(value.as_bytes()); data.push(0);
        data.resize(aligned(data.len(), 4), 0);
    }
    data
}

fn dirent(out: &mut Vec<u8>, inode: u32, kind: u32, name: &str) {
    let at = out.len(); let length = aligned(16 + name.len() + 1, 4);
    out.resize(at + length, 0);
    le32(out, at, inode); le32(out, at + 4, kind);
    le32(out, at + 8, name.len() as u32); le32(out, at + 12, length as u32);
    out[at + 16..at + 16 + name.len()].copy_from_slice(name.as_bytes());
}

fn inode(out: &mut [u8], index: usize, mode: u16, compressed: bool, length: usize, first: u32, blocks: u32, signed: bool) {
    let stride = if signed { 712 } else { 168 };
    let at = BLOCK + index * stride;
    le16(out, at, mode); le16(out, at + 2, 1);
    le32(out, at + 4, u32::from(compressed));
    le64(out, at + 8, length as u64); le64(out, at + 16, length as u64);
    le32(out, at + 96, blocks);
    let pointer = at + 100 + if signed { 32 } else { 0 };
    le32(out, pointer, first);
    le32(out, pointer + if signed { 36 } else { 4 }, u32::MAX);
}

pub fn pfs(files: &[(String, Vec<u8>, bool)], encrypted: bool, signed: bool) -> Vec<u8> {
    let count = files.len() + 2;
    let blocks = 4 + files.iter().map(|(_, data, _)| data.len().div_ceil(BLOCK)).sum::<usize>();
    let mut output = vec![0u8; blocks * BLOCK];
    le64(&mut output, 0, 1); le64(&mut output, 8, 20130315);
    le16(&mut output, 0x1c, u16::from(signed) | if encrypted { 4 } else { 0 });
    le32(&mut output, 0x20, BLOCK as u32); le64(&mut output, 0x28, blocks as u64);
    le64(&mut output, 0x30, count as u64); le64(&mut output, 0x38, blocks as u64 - 2);
    le64(&mut output, 0x40, 1); le64(&mut output, 0x48, 0);
    output[0x370..0x380].fill(0x55);
    let mut superroot = Vec::new(); dirent(&mut superroot, 1, 3, "uroot");
    output[2 * BLOCK..2 * BLOCK + superroot.len()].copy_from_slice(&superroot);
    inode(&mut output, 0, 0x4000, false, superroot.len(), 2, 1, signed);
    let mut root = Vec::new();
    let mut cursor = 4;
    for (n, (name, data, compressed)) in files.iter().enumerate() {
        dirent(&mut root, n as u32 + 2, 2, name);
        let needed = data.len().div_ceil(BLOCK);
        inode(&mut output, n + 2, 0x8000, *compressed, data.len(), cursor as u32, needed as u32, signed);
        output[cursor * BLOCK..cursor * BLOCK + data.len()].copy_from_slice(data);
        cursor += needed;
    }
    output[3 * BLOCK..3 * BLOCK + root.len()].copy_from_slice(&root);
    inode(&mut output, 1, 0x4000, false, root.len(), 3, 1, signed);
    output
}

pub fn pfsc(source: &[u8]) -> Vec<u8> {
    let blocks = source.len().div_ceil(BLOCK);
    let table = 0x30;
    let data_start = aligned(table + (blocks + 1) * 8, 16);
    let mut output = vec![0u8; data_start];
    output[..4].copy_from_slice(b"PFSC");
    le32(&mut output, 0x0c, BLOCK as u32); le64(&mut output, 0x10, BLOCK as u64);
    le64(&mut output, 0x18, table as u64); le64(&mut output, 0x20, data_start as u64);
    le64(&mut output, 0x28, source.len() as u64);
    for (n, chunk) in source.chunks(BLOCK).enumerate() {
        let offset = output.len(); le64(&mut output, table + n * 8, offset as u64);
        let mut encoder = ZlibEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(chunk).unwrap();
        let compressed = encoder.finish().unwrap();
        if compressed.len() < chunk.len() { output.extend_from_slice(&compressed); }
        else { output.extend_from_slice(chunk); }
    }
    let end = output.len(); le64(&mut output, table + blocks * 8, end as u64);
    output
}

pub struct Options<'a> {
    pub title: &'a str,
    pub update: bool,
    pub pfs_offset: u64,
    pub file_name: Option<&'a str>,
    pub executable_size: Option<usize>,
    pub invalid_pfs_geometry: bool,
    pub corrupt_file_data: bool,
    pub signed: bool,
}
impl Default for Options<'_> {
    fn default() -> Self {
        Self { title: "CUSA00093", update: false, pfs_offset: 0x10000, file_name: None,
            executable_size: None, invalid_pfs_geometry: false, corrupt_file_data: false, signed: true }
    }
}

pub fn write_fixture(path: &Path, options: Options<'_>) -> io::Result<File> {
    let key3 = [0x42u8; 32]; let ekpfs = [0x33u8; 32];
    let mut random = StdRng::seed_from_u64(0xdc000093);
    let mut files = vec![("eboot.bin".into(),
        options.executable_size.map(|n| vec![0x91; n]).unwrap_or_else(||
            if options.update { UPDATE_EBOOT.to_vec() } else { BASE_EBOOT.to_vec() }), false)];
    files.push((options.file_name.unwrap_or(if options.update { "update-only.bin" } else { "game000.dat" }).into(),
        if options.update { UPDATE_DATA.to_vec() } else { BASE_ARCHIVE.to_vec() }, false));
    let mut inner = pfs(&files, false, options.signed);
    if options.invalid_pfs_geometry { le64(&mut inner, 0x30, u64::MAX); }
    let mut compressed = pfsc(&inner);
    if options.corrupt_file_data {
        // Damage the first file's compressed block while retaining readable PFS metadata.
        let offset = u64::from_le_bytes(compressed[0x30 + 4 * 8..0x30 + 5 * 8].try_into().unwrap()) as usize;
        compressed[offset] ^= 0xff;
    }
    let mut outer = pfs(&[("pfs_image.dat".into(), compressed, true)], true, options.signed);
    let mut hmac = <Hmac<Sha256> as Mac>::new_from_slice(&ekpfs).unwrap();
    hmac.update(&[1, 0, 0, 0]); hmac.update(&[0x55; 16]);
    let secret = hmac.finalize().into_bytes();
    let xts = Xts128::new(Aes128::new_from_slice(&secret[16..]).unwrap(), Aes128::new_from_slice(&secret[..16]).unwrap());
    for (n, sector) in outer.chunks_exact_mut(0x1000).enumerate().skip(BLOCK / 0x1000) {
        xts.encrypt_sector(sector, get_tweak_default(n as u128));
    }
    let package_size = options.pfs_offset + outer.len() as u64;
    let mut header = vec![0u8; 0x1000];
    be32(&mut header, 0, 0x7f434e54); be32(&mut header, 4, 0x81000001);
    be32(&mut header, 0xc, 3); be32(&mut header, 0x10, 3); be32(&mut header, 0x18, 0x1800);
    let content = format!("UP9000-{}_00-DRIVECLUBTEST000", options.title);
    header[0x40..0x64].copy_from_slice(content.as_bytes());
    be32(&mut header, 0x70, 0xf); be32(&mut header, 0x74, 0x1a);
    be32(&mut header, 0x78, if options.update { 0x60000000 } else { 0x02000000 });
    be32(&mut header, 0x404, 1); be64(&mut header, 0x408, 0x80000000000003cc);
    be64(&mut header, 0x410, options.pfs_offset); be64(&mut header, 0x418, outer.len() as u64);
    be64(&mut header, 0x430, package_size);
    header[0x440..0x460].copy_from_slice(&Sha256::digest(&outer));
    let metadata = sfo(options.title, options.update);
    let mut table = vec![0u8; 3 * 32];
    for (n, (id, offset, size, encrypted)) in [(0x10, 0x2000, 2048, false), (0x20, 0x2800, 256, true),
                                            (0x1000, 0x2900, metadata.len(), false)].into_iter().enumerate() {
        let entry = n * 32;
        be32(&mut table, entry, id); be32(&mut table, entry + 8, if encrypted { 0x80000000 } else { 0 });
        be32(&mut table, entry + 12, if encrypted { 0x3000 } else { 0 });
        be32(&mut table, entry + 16, offset); be32(&mut table, entry + 20, size as u32);
    }
    let mut keys = vec![0u8; 2048];
    let encrypted = RsaPublicKey::from(orbis_pkg::keys::pkg_key3()).encrypt(&mut random, Pkcs1v15Encrypt, &key3).unwrap();
    keys[256 + 3 * 256..256 + 4 * 256].copy_from_slice(&encrypted);
    let mut image_key = RsaPublicKey::from(orbis_pkg::keys::fake_pfs_key()).encrypt(&mut random, Pkcs1v15Encrypt, &ekpfs).unwrap();
    let mut hash = Sha256::new(); hash.update(&table[32..64]); hash.update(key3);
    let key_iv = hash.finalize();
    let mut cipher = cbc::Encryptor::<Aes128>::new_from_slices(&key_iv[16..], &key_iv[..16]).unwrap();
    for block in image_key.chunks_exact_mut(16) { cipher.encrypt_block_mut(block.into()); }
    let file = OpenOptions::new().read(true).write(true).create_new(true).open(path)?;
    file.set_len(package_size)?;
    file.write_all_at(&header, 0)?; file.write_all_at(&table, 0x1800)?;
    file.write_all_at(&keys, 0x2000)?; file.write_all_at(&image_key, 0x2800)?;
    file.write_all_at(&metadata, 0x2900)?; file.write_all_at(&outer, options.pfs_offset)?;
    file.sync_all()?;
    Ok(file)
}
