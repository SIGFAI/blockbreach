//! Everything BlockBreach does to the PC: finding the games, installing / repairing / removing the mod, the settings
//! file the RoN mod reads, and starting both games. No UI here: long jobs report through `Progress`.

use std::collections::BTreeMap;
use std::fs;
use std::io::{Cursor, Read};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use winreg::enums::*;
use winreg::RegKey;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
pub const RON_APP_ID: &str = "1144200";
pub const RON_EXE: &str = "ReadyOrNotSteam-Win64-Shipping.exe";
pub const FABRIC_VERSION: &str = "fabric-loader-0.19.5-26.3";
pub const PROFILE_ID: &str = "blockbreach";
const PAYLOAD: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/build/payload.zip"));
const PROFILE_ICON: &str = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/assets/profile_icon.txt"));
const STORE_PACKAGE: &str = "Microsoft.4297127D64EC6_8wekyb3d8bbwe";
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

// ---------------------------------------------------------------------------------------------------------------
// state kept between runs: %LOCALAPPDATA%\BlockBreach\state.json

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Settings {
    pub mc_scale: u32,
    pub mobs_hunt_player: bool,
    pub squad_fights_mobs: bool,
    pub dedicated_gpu: bool,
    pub desktop_shortcut: bool,
    pub keep_world: bool,
    pub language: String,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            mc_scale: 75,
            mobs_hunt_player: true,
            squad_fights_mobs: true,
            dedicated_gpu: true,
            desktop_shortcut: true,
            keep_world: true,
            language: String::new(),
        }
    }
}

/// What an install put where, so repair and uninstall touch only that.
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct Installed {
    pub version: String,
    pub ron_win64: PathBuf,
    /// files written into Win64 (relative paths)
    pub ron_files: Vec<String>,
    /// files that were there before and were moved to the backup folder (relative paths)
    pub ron_backups: Vec<String>,
    /// directories under Win64 that didn't exist before (relative, deepest last)
    pub ron_new_dirs: Vec<String>,
    pub mc_dir: PathBuf,
    pub game_dir: PathBuf,
    pub fabric_version_created: bool,
    /// GPU preference values set: exe path -> the value that was there before ("" = none)
    pub gpu_prefs: BTreeMap<String, String>,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct State {
    pub settings: Settings,
    pub ron_dir: Option<PathBuf>,
    pub mc_dir: Option<PathBuf>,
    pub installed: Option<Installed>,
}

/// Self-test mode (`--selftest`): everything under a scratch folder, no registry, shortcuts or process checks.
pub fn selftest() -> bool {
    std::env::var_os("BLOCKBREACH_SELFTEST").is_some()
}

pub fn app_dir() -> PathBuf {
    if let Some(home) = std::env::var_os("BLOCKBREACH_HOME") {
        return PathBuf::from(home);
    }
    let base = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    base.join("BlockBreach")
}

impl State {
    pub fn load() -> State {
        fs::read_to_string(app_dir().join("state.json"))
            .ok()
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default()
    }

    pub fn save(&self) {
        let dir = app_dir();
        let _ = fs::create_dir_all(&dir);
        if let Ok(text) = serde_json::to_string_pretty(self) {
            let _ = fs::write(dir.join("state.json"), text);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// progress shared with the UI

#[derive(Clone, Debug, PartialEq)]
pub enum LineKind {
    Info,
    Ok,
    Warn,
    Error,
}

#[derive(Default)]
pub struct ProgressInner {
    pub running: bool,
    pub fraction: f32,
    pub stage: String,
    pub lines: Vec<(LineKind, String)>,
    pub done: Option<Result<String, String>>,
    /// the job failed for lack of rights: offer "restart as administrator"
    pub needs_admin: bool,
}

#[derive(Clone, Default)]
pub struct Progress(pub Arc<Mutex<ProgressInner>>);

impl Progress {
    pub fn start(&self) {
        let mut p = self.0.lock().unwrap();
        *p = ProgressInner { running: true, ..Default::default() };
    }
    pub fn stage(&self, fraction: f32, text: impl Into<String>) {
        let mut p = self.0.lock().unwrap();
        p.fraction = fraction;
        let t = text.into();
        p.lines.push((LineKind::Info, t.clone()));
        p.stage = t;
    }
    pub fn line(&self, kind: LineKind, text: impl Into<String>) {
        self.0.lock().unwrap().lines.push((kind, text.into()));
    }
    pub fn finish(&self, result: Result<String, String>) {
        let mut p = self.0.lock().unwrap();
        p.running = false;
        if result.is_ok() {
            p.fraction = 1.0;
        }
        match &result {
            Ok(m) => p.lines.push((LineKind::Ok, m.clone())),
            Err(e) => p.lines.push((LineKind::Error, e.clone())),
        }
        p.done = Some(result);
    }
    pub fn needs_admin(&self) {
        self.0.lock().unwrap().needs_admin = true;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// finding the games

/// Ready or Not's install folder (the one holding ReadyOrNot\Binaries\Win64), from Steam's libraries.
pub fn find_ron() -> Option<PathBuf> {
    for lib in steam_libraries() {
        let manifest = lib.join("steamapps").join(format!("appmanifest_{RON_APP_ID}.acf"));
        let dir = fs::read_to_string(&manifest)
            .ok()
            .and_then(|t| vdf_value(&t, "installdir"))
            .map(|d| lib.join("steamapps").join("common").join(d))
            .unwrap_or_else(|| lib.join("steamapps").join("common").join("Ready Or Not"));
        if ron_valid(&dir) {
            return Some(dir);
        }
    }
    None
}

pub fn ron_valid(dir: &Path) -> bool {
    win64(dir).join(RON_EXE).is_file()
}

pub fn win64(ron: &Path) -> PathBuf {
    ron.join("ReadyOrNot").join("Binaries").join("Win64")
}

pub fn steam_dir() -> Option<PathBuf> {
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    if let Ok(k) = hkcu.open_subkey("Software\\Valve\\Steam") {
        if let Ok(p) = k.get_value::<String, _>("SteamPath") {
            return Some(PathBuf::from(p.replace('/', "\\")));
        }
    }
    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    for key in ["SOFTWARE\\WOW6432Node\\Valve\\Steam", "SOFTWARE\\Valve\\Steam"] {
        if let Ok(k) = hklm.open_subkey(key) {
            if let Ok(p) = k.get_value::<String, _>("InstallPath") {
                return Some(PathBuf::from(p));
            }
        }
    }
    let default = PathBuf::from("C:\\Program Files (x86)\\Steam");
    default.is_dir().then_some(default)
}

fn steam_libraries() -> Vec<PathBuf> {
    let mut libs = Vec::new();
    if let Some(steam) = steam_dir() {
        libs.push(steam.clone());
        if let Ok(text) = fs::read_to_string(steam.join("steamapps").join("libraryfolders.vdf")) {
            for line in text.lines() {
                let t = line.trim();
                if t.starts_with("\"path\"") {
                    if let Some(v) = t.split('"').nth(3) {
                        let p = PathBuf::from(v.replace("\\\\", "\\"));
                        if !libs.contains(&p) {
                            libs.push(p);
                        }
                    }
                }
            }
        }
    }
    libs
}

fn vdf_value(text: &str, key: &str) -> Option<String> {
    let needle = format!("\"{key}\"");
    text.lines().map(str::trim).find(|l| l.starts_with(&needle)).and_then(|l| l.split('"').nth(3).map(str::to_string))
}

pub fn default_mc_dir() -> PathBuf {
    let roaming = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_default();
    roaming.join(".minecraft")
}

#[derive(Clone, Debug, PartialEq)]
pub enum Launcher {
    Store,
    Exe(PathBuf),
}

/// The official Minecraft Launcher: the Microsoft Store / Xbox app one, or the classic exe.
pub fn find_launcher() -> Option<Launcher> {
    let local = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_default();
    if local.join("Packages").join(STORE_PACKAGE).is_dir() {
        return Some(Launcher::Store);
    }
    let mut candidates = vec![
        PathBuf::from("C:\\XboxGames\\Minecraft Launcher\\Content\\Minecraft.exe"),
        PathBuf::from("C:\\Program Files (x86)\\Minecraft Launcher\\MinecraftLauncher.exe"),
        PathBuf::from("C:\\Program Files\\Minecraft Launcher\\MinecraftLauncher.exe"),
    ];
    if let Some(pf) = std::env::var_os("ProgramFiles(x86)") {
        candidates.push(PathBuf::from(pf).join("Minecraft Launcher").join("MinecraftLauncher.exe"));
    }
    candidates.into_iter().find(|p| p.is_file()).map(Launcher::Exe)
}

// ---------------------------------------------------------------------------------------------------------------
// processes and windows

pub fn process_running(exe: &str) -> bool {
    process_ids(exe).next().is_some()
}

fn process_ids(exe: &str) -> impl Iterator<Item = u32> {
    use windows_sys::Win32::System::Diagnostics::ToolHelp::*;
    let mut ids = Vec::new();
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if !snap.is_null() && snap as isize != -1 {
            let mut entry: PROCESSENTRY32W = std::mem::zeroed();
            entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
            let mut ok = Process32FirstW(snap, &mut entry);
            while ok != 0 {
                let len = entry.szExeFile.iter().position(|&c| c == 0).unwrap_or(entry.szExeFile.len());
                let name = String::from_utf16_lossy(&entry.szExeFile[..len]);
                if name.eq_ignore_ascii_case(exe) {
                    ids.push(entry.th32ProcessID);
                }
                ok = Process32NextW(snap, &mut entry);
            }
            windows_sys::Win32::Foundation::CloseHandle(snap);
        }
    }
    ids.into_iter()
}

/// Titles of the visible top-level windows of processes named `exe`.
pub fn window_titles(exe: &str) -> Vec<String> {
    windows_of(exe).into_iter().map(|(_, t)| t).collect()
}

/// The visible top-level windows (handle, title) of processes named `exe`.
fn windows_of(exe: &str) -> Vec<(isize, String)> {
    use windows_sys::Win32::Foundation::{BOOL, HWND, LPARAM};
    use windows_sys::Win32::UI::WindowsAndMessaging::*;
    struct Ctx {
        pids: Vec<u32>,
        titles: Vec<(isize, String)>,
    }
    unsafe extern "system" fn each(hwnd: HWND, lp: LPARAM) -> BOOL {
        let ctx = &mut *(lp as *mut Ctx);
        let mut pid = 0u32;
        GetWindowThreadProcessId(hwnd, &mut pid);
        if ctx.pids.contains(&pid) && IsWindowVisible(hwnd) != 0 {
            let mut buf = [0u16; 256];
            let n = GetWindowTextW(hwnd, buf.as_mut_ptr(), 256);
            if n > 0 {
                ctx.titles.push((hwnd as isize, String::from_utf16_lossy(&buf[..n as usize])));
            }
        }
        1
    }
    let mut ctx = Ctx { pids: process_ids(exe).collect(), titles: Vec::new() };
    if ctx.pids.is_empty() {
        return Vec::new();
    }
    unsafe {
        EnumWindows(Some(each), &mut ctx as *mut Ctx as LPARAM);
    }
    ctx.titles
}

/// The Minecraft Launcher's windows (the Store/Xbox one runs as Minecraft.exe, the classic one as MinecraftLauncher.exe).
fn launcher_windows() -> Vec<isize> {
    let mut out = Vec::new();
    for exe in ["Minecraft.exe", "MinecraftLauncher.exe"] {
        out.extend(windows_of(exe).into_iter().filter(|(_, t)| t.starts_with("Minecraft Launcher")).map(|(h, _)| h));
    }
    out
}

/// Closes an open Minecraft Launcher like its window's X, and waits for it to go. The launcher keeps the profiles in
/// memory: one left open neither shows a profile written meanwhile nor keeps it (it saves its own list on exit).
/// Never killed: killing it once left Windows' focus on a hidden system window.
pub fn close_launcher() -> bool {
    use windows_sys::Win32::UI::WindowsAndMessaging::{PostMessageW, WM_CLOSE};
    let windows = launcher_windows();
    if windows.is_empty() {
        return false;
    }
    for h in &windows {
        unsafe {
            PostMessageW(*h as _, WM_CLOSE, 0, 0);
        }
    }
    let _ = wait_for(Duration::from_secs(20), || launcher_windows().is_empty());
    std::thread::sleep(Duration::from_millis(1500)); // let it write its files before ours go in
    true
}

pub fn minecraft_window() -> Option<String> {
    window_titles("javaw.exe").into_iter().find(|t| t.starts_with("Minecraft"))
}

pub fn ron_running() -> bool {
    process_running(RON_EXE)
}

// ---------------------------------------------------------------------------------------------------------------
// install / repair

fn payload() -> Result<zip::ZipArchive<Cursor<&'static [u8]>>, String> {
    zip::ZipArchive::new(Cursor::new(PAYLOAD)).map_err(|e| format!("payload: {e}"))
}

fn payload_entries(prefix: &str) -> Result<Vec<(String, Vec<u8>)>, String> {
    let mut zip = payload()?;
    let mut out = Vec::new();
    for i in 0..zip.len() {
        let mut f = zip.by_index(i).map_err(|e| e.to_string())?;
        if f.is_dir() || !f.name().starts_with(prefix) {
            continue;
        }
        let name = f.name()[prefix.len()..].to_string();
        let mut data = Vec::with_capacity(f.size() as usize);
        f.read_to_end(&mut data).map_err(|e| e.to_string())?;
        out.push((name, data));
    }
    Ok(out)
}

pub fn license_texts() -> Vec<(String, String)> {
    payload_entries("licenses/")
        .unwrap_or_default()
        .into_iter()
        .map(|(n, d)| (n, String::from_utf8_lossy(&d).into_owned()))
        .collect()
}

fn rel_path(base: &Path, rel: &str) -> PathBuf {
    rel.split('/').fold(base.to_path_buf(), |p, part| p.join(part))
}

fn is_access_denied(e: &std::io::Error) -> bool {
    e.kind() == std::io::ErrorKind::PermissionDenied || e.raw_os_error() == Some(5)
}

pub struct Job {
    pub state: State,
    pub progress: Progress,
}

/// Install or repair. Returns the updated state.
pub fn install(mut job: Job) -> State {
    let p = job.progress.clone();
    let result = install_inner(&mut job.state, &p);
    match result {
        Ok(msg) => p.finish(Ok(msg)),
        Err(e) => {
            if e.contains("(os error 5)") || e.to_lowercase().contains("access is denied") || e.contains("PermissionDenied") {
                p.needs_admin();
            }
            p.finish(Err(e))
        }
    }
    job.state.save();
    job.state
}

fn install_inner(state: &mut State, p: &Progress) -> Result<String, String> {
    let ron = state.ron_dir.clone().filter(|d| ron_valid(d)).ok_or("Ready or Not was not found: pick its folder")?;
    let mc_dir = state.mc_dir.clone().unwrap_or_else(default_mc_dir);
    p.stage(0.02, "Checking that the games are closed");
    if !selftest() && ron_running() {
        return Err("Ready or Not is running: close it first".into());
    }
    if !selftest() && minecraft_window().is_some() {
        return Err("Minecraft is running: close it first".into());
    }

    let w64 = win64(&ron);
    let backup_dir = app_dir().join("backup").join("Win64");
    let mut inst = state.installed.clone().unwrap_or_default();
    let ours: std::collections::HashSet<String> = inst.ron_files.iter().cloned().collect();

    // --- Ready or Not
    p.stage(0.08, "Ready or Not: UE4SS, ReShade and the BlockBreach host mod");
    let entries = payload_entries("ron/")?;
    let n = entries.len().max(1) as f32;
    for (i, (rel, data)) in entries.iter().enumerate() {
        let dest = rel_path(&w64, rel);
        // directories that didn't exist before are ours entirely (removed on uninstall)
        let mut missing = Vec::new();
        let mut d = dest.parent().map(Path::to_path_buf);
        while let Some(dir) = d {
            if dir == w64 || dir.exists() {
                break;
            }
            missing.push(dir.strip_prefix(&w64).unwrap().to_string_lossy().replace('\\', "/"));
            d = dir.parent().map(Path::to_path_buf);
        }
        for m in missing.into_iter().rev() {
            if !inst.ron_new_dirs.contains(&m) {
                inst.ron_new_dirs.push(m);
            }
        }
        if dest.exists() && !ours.contains(rel) && !inst.ron_backups.contains(rel) {
            let b = rel_path(&backup_dir, rel);
            fs::create_dir_all(b.parent().unwrap()).map_err(|e| format!("backup {rel}: {e}"))?;
            fs::copy(&dest, &b).map_err(|e| format!("backup {rel}: {e}"))?;
            inst.ron_backups.push(rel.clone());
            p.line(LineKind::Warn, format!("kept a copy of the existing {rel}"));
        }
        if let Some(parent) = dest.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        }
        fs::write(&dest, data).map_err(|e| {
            if is_access_denied(&e) {
                p.needs_admin();
            }
            format!("{rel}: {e}")
        })?;
        if !inst.ron_files.contains(rel) {
            inst.ron_files.push(rel.clone());
        }
        let mut pr = p.0.lock().unwrap();
        pr.fraction = 0.08 + 0.42 * (i as f32 + 1.0) / n;
    }
    inst.ron_win64 = w64.clone();
    write_config(&w64, &state.settings).map_err(|e| format!("config.ini: {e}"))?;
    let cfg = "ue4ss/Mods/RoNPassthrough/config.ini".to_string();
    if !inst.ron_files.contains(&cfg) {
        inst.ron_files.push(cfg);
    }
    p.line(LineKind::Ok, format!("Ready or Not: {} files in {}", entries.len(), w64.display()));

    // --- Minecraft
    p.stage(0.55, "Minecraft: Fabric Loader, Fabric API and the BlockBreach mod");
    let game_dir = app_dir().join("minecraft");
    let mods = game_dir.join("mods");
    fs::create_dir_all(&mods).map_err(|e| format!("{}: {e}", mods.display()))?;
    // only our two jars: an old version of either would load twice
    if let Ok(list) = fs::read_dir(&mods) {
        for f in list.flatten() {
            let name = f.file_name().to_string_lossy().to_lowercase();
            if name.starts_with("blockbreach") || name.starts_with("fabric-api") {
                let _ = fs::remove_file(f.path());
            }
        }
    }
    for (rel, data) in payload_entries("mc/")? {
        if let Some(name) = rel.strip_prefix("mods/") {
            fs::write(mods.join(name), &data).map_err(|e| format!("{name}: {e}"))?;
        } else if rel == "options.txt" {
            let dest = game_dir.join("options.txt");
            if !dest.exists() {
                fs::write(&dest, &data).map_err(|e| format!("options.txt: {e}"))?;
            }
        } else if let Some(v) = rel.strip_prefix("versions/") {
            let dest = rel_path(&mc_dir.join("versions"), v);
            let dir = dest.parent().unwrap().to_path_buf();
            if !dir.exists() {
                inst.fabric_version_created = true;
            }
            fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
            fs::write(&dest, &data).map_err(|e| format!("{v}: {e}"))?;
            // the launcher fills this jar from the vanilla one on first launch; it only has to exist
            let jar = dir.join(format!("{FABRIC_VERSION}.jar"));
            if !jar.exists() {
                const EMPTY_ZIP: [u8; 22] = [0x50, 0x4b, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
                fs::write(&jar, EMPTY_ZIP).map_err(|e| format!("{}: {e}", jar.display()))?;
            }
        }
    }
    inst.mc_dir = mc_dir.clone();
    inst.game_dir = game_dir.clone();
    p.line(LineKind::Ok, format!("Minecraft mods in {}", mods.display()));

    p.stage(0.72, "Minecraft Launcher: the BlockBreach profile");
    if !selftest() && close_launcher() {
        p.line(LineKind::Warn, "closed the Minecraft Launcher (it only sees new profiles when it starts)");
    }
    add_profile(&mc_dir, &game_dir, true)?;
    p.line(LineKind::Ok, "profile \"BlockBreach\" added (Minecraft 26.3 + Fabric)");

    // --- GPU
    if state.settings.dedicated_gpu && !selftest() {
        p.stage(0.8, "Windows graphics settings: both games on the dedicated GPU");
        let mut exes = vec![w64.join(RON_EXE)];
        exes.extend(java_runtimes(&mc_dir));
        for exe in exes {
            let key = exe.to_string_lossy().to_string();
            match set_gpu_pref(&key) {
                Ok(previous) => {
                    inst.gpu_prefs.entry(key).or_insert(previous);
                }
                Err(e) => p.line(LineKind::Warn, format!("GPU preference for {key}: {e}")),
            }
        }
    }

    // --- the app itself, shortcuts, Apps & features entry
    p.stage(0.88, "Shortcuts and the Windows uninstall entry");
    let exe = install_self().map_err(|e| format!("copying BlockBreach.exe: {e}"))?;
    if selftest() {
        inst.version = VERSION.to_string();
        state.installed = Some(inst);
        state.ron_dir = Some(ron);
        state.mc_dir = Some(mc_dir);
        return Ok("BlockBreach is installed (self-test: no shortcuts)".into());
    }
    let start_menu = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_default().join("Microsoft\\Windows\\Start Menu\\Programs");
    let _ = make_shortcut(&exe, &start_menu.join("BlockBreach.lnk"));
    if state.settings.desktop_shortcut {
        if let Some(desktop) = desktop_dir() {
            let _ = make_shortcut(&exe, &desktop.join("BlockBreach.lnk"));
        }
    }
    register_uninstall(&exe).map_err(|e| format!("uninstall entry: {e}"))?;

    inst.version = VERSION.to_string();
    state.installed = Some(inst);
    state.ron_dir = Some(ron);
    state.mc_dir = Some(mc_dir);
    Ok("BlockBreach is installed".into())
}

/// The RoN mod's settings file (read once when RoN starts).
pub fn write_config(w64: &Path, s: &Settings) -> std::io::Result<()> {
    let dir = w64.join("ue4ss").join("Mods").join("RoNPassthrough");
    fs::create_dir_all(&dir)?;
    fs::write(
        dir.join("config.ini"),
        format!(
            "; written by BlockBreach {VERSION}; read when Ready or Not starts\r\n[blockbreach]\r\nmc_scale={}\r\nmobs_hunt_player={}\r\nsquad_fights_mobs={}\r\n",
            s.mc_scale.clamp(25, 100),
            s.mobs_hunt_player as u8,
            s.squad_fights_mobs as u8
        ),
    )
}

fn profiles_path(mc_dir: &Path) -> PathBuf {
    mc_dir.join("launcher_profiles.json")
}

fn now_iso() -> String {
    use windows_sys::Win32::System::SystemInformation::GetSystemTime;
    let t = unsafe {
        let mut t = std::mem::zeroed();
        GetSystemTime(&mut t);
        t
    };
    format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds)
}

/// Adds (or refreshes) the BlockBreach profile. `select`: newest lastUsed, so the launcher opens with it chosen.
pub fn add_profile(mc_dir: &Path, game_dir: &Path, select: bool) -> Result<(), String> {
    let path = profiles_path(mc_dir);
    fs::create_dir_all(mc_dir).map_err(|e| e.to_string())?;
    let mut json: serde_json::Value = match fs::read_to_string(&path) {
        Ok(t) => {
            let backup = app_dir().join("launcher_profiles.before-blockbreach.json");
            if !backup.exists() {
                let _ = fs::create_dir_all(app_dir());
                let _ = fs::write(&backup, &t);
            }
            serde_json::from_str(t.trim_start_matches('\u{feff}')).map_err(|e| format!("launcher_profiles.json: {e}"))?
        }
        Err(_) => serde_json::json!({ "profiles": {}, "settings": {}, "version": 3 }),
    };
    let now = now_iso();
    let profiles = json
        .as_object_mut()
        .ok_or("launcher_profiles.json is not an object")?
        .entry("profiles")
        .or_insert_with(|| serde_json::json!({}));
    let existing_created = profiles.get(PROFILE_ID).and_then(|p| p.get("created")).cloned();
    profiles[PROFILE_ID] = serde_json::json!({
        "created": existing_created.unwrap_or_else(|| serde_json::Value::String(now.clone())),
        "lastUsed": if select { now.clone() } else { "1970-01-02T00:00:00.000Z".to_string() },
        "icon": PROFILE_ICON.trim(),
        "lastVersionId": FABRIC_VERSION,
        "name": "BlockBreach",
        "type": "custom",
        "gameDir": game_dir.to_string_lossy(),
        "javaArgs": "-Xmx4G -XX:+UnlockExperimentalVMOptions -XX:+UseG1GC -XX:G1NewSizePercent=20 -XX:G1ReservePercent=20 -XX:MaxGCPauseMillis=50",
    });
    // UTF-8 without a BOM: the launcher's parser is strict
    let text = serde_json::to_string_pretty(&json).map_err(|e| e.to_string())?;
    fs::write(&path, text).map_err(|e| format!("launcher_profiles.json: {e}"))
}

fn remove_profile(mc_dir: &Path) -> Result<(), String> {
    let path = profiles_path(mc_dir);
    let Ok(t) = fs::read_to_string(&path) else { return Ok(()) };
    let mut json: serde_json::Value = serde_json::from_str(t.trim_start_matches('\u{feff}')).map_err(|e| e.to_string())?;
    if let Some(p) = json.get_mut("profiles").and_then(|p| p.as_object_mut()) {
        p.remove(PROFILE_ID);
    }
    fs::write(&path, serde_json::to_string_pretty(&json).map_err(|e| e.to_string())?).map_err(|e| e.to_string())
}

/// The launcher's Java runtimes (Minecraft 26.3 runs on java-runtime-epsilon), in both places launchers keep them.
fn java_runtimes(mc_dir: &Path) -> Vec<PathBuf> {
    let mut roots = vec![mc_dir.join("runtime")];
    if let Some(local) = std::env::var_os("LOCALAPPDATA") {
        roots.push(PathBuf::from(local).join("Packages").join(STORE_PACKAGE).join("LocalCache").join("Local").join("runtime"));
    }
    let mut out = Vec::new();
    for root in roots {
        let Ok(list) = fs::read_dir(&root) else { continue };
        for rt in list.flatten() {
            // runtime\<component>\windows-x64\<component>\bin\javaw.exe
            let name = rt.file_name();
            let exe = rt.path().join("windows-x64").join(&name).join("bin").join("javaw.exe");
            if exe.is_file() {
                out.push(exe);
            }
        }
    }
    out
}

const GPU_KEY: &str = "Software\\Microsoft\\DirectX\\UserGpuPreferences";

/// Windows "Graphics settings": this exe on the high-performance GPU. Returns the value that was there.
fn set_gpu_pref(exe: &str) -> std::io::Result<String> {
    let (key, _) = RegKey::predef(HKEY_CURRENT_USER).create_subkey(GPU_KEY)?;
    let previous: String = key.get_value(exe).unwrap_or_default();
    key.set_value(exe, &"GpuPreference=2;")?;
    Ok(previous)
}

fn restore_gpu_pref(exe: &str, previous: &str) {
    if let Ok(key) = RegKey::predef(HKEY_CURRENT_USER).open_subkey_with_flags(GPU_KEY, KEY_ALL_ACCESS) {
        if previous.is_empty() {
            let _ = key.delete_value(exe);
        } else {
            let _ = key.set_value(exe, &previous);
        }
    }
}

fn install_self() -> std::io::Result<PathBuf> {
    let me = std::env::current_exe()?;
    let dir = app_dir();
    fs::create_dir_all(&dir)?;
    let dest = dir.join("BlockBreach.exe");
    if !same_file(&me, &dest) {
        fs::copy(&me, &dest)?;
    }
    Ok(dest)
}

fn same_file(a: &Path, b: &Path) -> bool {
    match (fs::canonicalize(a), fs::canonicalize(b)) {
        (Ok(x), Ok(y)) => x == y,
        _ => false,
    }
}

pub fn running_from_app_dir() -> bool {
    std::env::current_exe().map(|me| same_file(&me, &app_dir().join("BlockBreach.exe"))).unwrap_or(false)
}

fn desktop_dir() -> Option<PathBuf> {
    let k = RegKey::predef(HKEY_CURRENT_USER)
        .open_subkey("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders")
        .ok()?;
    let raw: String = k.get_value("Desktop").ok()?;
    let profile = std::env::var("USERPROFILE").unwrap_or_default();
    Some(PathBuf::from(raw.replace("%USERPROFILE%", &profile)))
}

fn powershell(script: &str) -> std::io::Result<bool> {
    use std::os::windows::process::CommandExt;
    Command::new("powershell.exe")
        .args(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script])
        .creation_flags(CREATE_NO_WINDOW)
        .status()
        .map(|s| s.success())
}

fn make_shortcut(target: &Path, link: &Path) -> std::io::Result<bool> {
    let q = |p: &Path| p.to_string_lossy().replace('\'', "''");
    powershell(&format!(
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{}');$s.TargetPath='{}';$s.WorkingDirectory='{}';$s.IconLocation='{},0';$s.Description='BlockBreach: Ready or Not x Minecraft';$s.Save()",
        q(link),
        q(target),
        q(target.parent().unwrap_or(Path::new("."))),
        q(target)
    ))
}

const UNINSTALL_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BlockBreach";

fn register_uninstall(exe: &Path) -> std::io::Result<()> {
    let (k, _) = RegKey::predef(HKEY_CURRENT_USER).create_subkey(UNINSTALL_KEY)?;
    let e = exe.to_string_lossy().to_string();
    k.set_value("DisplayName", &"BlockBreach (Ready or Not x Minecraft)")?;
    k.set_value("DisplayVersion", &VERSION)?;
    k.set_value("Publisher", &"BlockBreach")?;
    k.set_value("DisplayIcon", &format!("{e},0"))?;
    k.set_value("InstallLocation", &app_dir().to_string_lossy().to_string())?;
    k.set_value("UninstallString", &format!("\"{e}\" --uninstall"))?;
    k.set_value("NoModify", &1u32)?;
    k.set_value("NoRepair", &1u32)?;
    Ok(())
}

// ---------------------------------------------------------------------------------------------------------------
// uninstall

pub fn uninstall(mut job: Job) -> State {
    let p = job.progress.clone();
    match uninstall_inner(&mut job.state, &p) {
        Ok(m) => p.finish(Ok(m)),
        Err(e) => {
            if e.contains("(os error 5)") {
                p.needs_admin();
            }
            p.finish(Err(e))
        }
    }
    job.state.save();
    job.state
}

fn uninstall_inner(state: &mut State, p: &Progress) -> Result<String, String> {
    let inst = state.installed.clone().ok_or("BlockBreach is not installed")?;
    p.stage(0.05, "Checking that the games are closed");
    if !selftest() && ron_running() {
        return Err("Ready or Not is running: close it first".into());
    }
    if !selftest() && minecraft_window().is_some() {
        return Err("Minecraft is running: close it first".into());
    }
    let w64 = inst.ron_win64.clone();
    p.stage(0.15, "Ready or Not: removing the mod's files");
    if w64.is_dir() {
        for rel in &inst.ron_files {
            let f = rel_path(&w64, rel);
            if f.exists() {
                fs::remove_file(&f).map_err(|e| format!("{rel}: {e}"))?;
            }
        }
        // the logs UE4SS and ReShade write next to themselves
        for extra in ["ReShade.log", "ReShade.log1", "ReShade.log2", "ReShade.log3", "ReShade.log4", "ue4ss/UE4SS.log"] {
            if !inst.ron_backups.iter().any(|b| b == extra) {
                let _ = fs::remove_file(rel_path(&w64, extra));
            }
        }
        // directories created by the install go with everything in them (history.log, shader caches)
        let mut dirs = inst.ron_new_dirs.clone();
        dirs.sort_by_key(|d| d.len());
        for d in &dirs {
            let path = rel_path(&w64, d);
            if path.is_dir() {
                let _ = fs::remove_dir_all(&path);
            }
        }
        p.stage(0.4, "Ready or Not: putting back what was there before");
        let backup_dir = app_dir().join("backup").join("Win64");
        for rel in &inst.ron_backups {
            let from = rel_path(&backup_dir, rel);
            let to = rel_path(&w64, rel);
            if from.exists() {
                if let Some(parent) = to.parent() {
                    let _ = fs::create_dir_all(parent);
                }
                fs::copy(&from, &to).map_err(|e| format!("restoring {rel}: {e}"))?;
            }
        }
        let _ = fs::remove_dir_all(app_dir().join("backup"));
    }

    p.stage(0.6, "Minecraft: the profile and the mods");
    if !selftest() {
        close_launcher();
    }
    remove_profile(&inst.mc_dir)?;
    if inst.fabric_version_created {
        let _ = fs::remove_dir_all(inst.mc_dir.join("versions").join(FABRIC_VERSION));
    }
    if inst.game_dir.is_dir() {
        if state.settings.keep_world {
            if let Ok(list) = fs::read_dir(&inst.game_dir) {
                for f in list.flatten() {
                    if f.file_name() != "saves" {
                        let path = f.path();
                        let _ = if path.is_dir() { fs::remove_dir_all(&path) } else { fs::remove_file(&path) };
                    }
                }
            }
        } else {
            let _ = fs::remove_dir_all(&inst.game_dir);
        }
    }

    if selftest() {
        state.installed = None;
        return Ok("BlockBreach is removed (self-test)".into());
    }
    p.stage(0.8, "Windows: graphics settings, shortcuts, uninstall entry");
    for (exe, previous) in &inst.gpu_prefs {
        restore_gpu_pref(exe, previous);
    }
    let start_menu = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_default().join("Microsoft\\Windows\\Start Menu\\Programs");
    let _ = fs::remove_file(start_menu.join("BlockBreach.lnk"));
    if let Some(desktop) = desktop_dir() {
        let _ = fs::remove_file(desktop.join("BlockBreach.lnk"));
    }
    let _ = RegKey::predef(HKEY_CURRENT_USER).delete_subkey_all(UNINSTALL_KEY);
    state.installed = None;
    Ok("BlockBreach is removed. Both games are back to how they were".into())
}

/// After an uninstall run from the installed copy: delete that copy once this process has exited.
pub fn schedule_self_delete() {
    use std::os::windows::process::CommandExt;
    let dir = app_dir();
    let keep_state = dir.join("state.json");
    let _ = keep_state;
    let cmd = format!(
        "ping -n 3 127.0.0.1 >nul & del /f /q \"{}\" & del /f /q \"{}\" & rmdir \"{}\"",
        dir.join("BlockBreach.exe").display(),
        dir.join("state.json").display(),
        dir.display()
    );
    let _ = Command::new("cmd.exe").args(["/c", &cmd]).creation_flags(CREATE_NO_WINDOW).spawn();
}

// ---------------------------------------------------------------------------------------------------------------
// play

#[derive(Clone, Debug, PartialEq)]
pub enum PlayStep {
    Idle,
    StartingLauncher,
    WaitingForMinecraft,
    WaitingForWorld,
    StartingRon,
    WaitingForRon,
    Done,
    Failed(String),
}

#[derive(Clone)]
pub struct Play(pub Arc<Mutex<PlayStep>>);

impl Play {
    pub fn new() -> Play {
        Play(Arc::new(Mutex::new(PlayStep::Idle)))
    }
    pub fn get(&self) -> PlayStep {
        self.0.lock().unwrap().clone()
    }
    fn set(&self, s: PlayStep) {
        *self.0.lock().unwrap() = s;
    }
}

/// Minecraft first (the launcher opens with the BlockBreach profile chosen; the player presses PLAY there, the mod
/// opens its world by itself), then Ready or Not in DirectX 11 through Steam.
pub fn play(state: State, play: Play) {
    let run = || -> Result<(), String> {
        let inst = state.installed.clone().ok_or("BlockBreach is not installed")?;
        if minecraft_window().is_none() {
            play.set(PlayStep::StartingLauncher);
            // an open launcher would neither select the profile nor even show one added while it ran
            close_launcher();
            add_profile(&inst.mc_dir, &inst.game_dir, true)?;
            let launcher = find_launcher().ok_or("the Minecraft Launcher was not found")?;
            start_launcher(&launcher)?;
            // the Xbox app's "Launching game..." splash (gamingservicesui) sometimes never hands over to the launcher:
            // close the splash and ask again, once
            if launcher == Launcher::Store && wait_for(Duration::from_secs(60), || !launcher_windows().is_empty()).is_none() {
                use windows_sys::Win32::UI::WindowsAndMessaging::{PostMessageW, WM_CLOSE};
                for (h, t) in windows_of("gamingservicesui.exe") {
                    if t.starts_with("Minecraft Launcher") {
                        unsafe {
                            PostMessageW(h as _, WM_CLOSE, 0, 0);
                        }
                    }
                }
                std::thread::sleep(Duration::from_secs(3));
                start_launcher(&launcher)?;
            }
            play.set(PlayStep::WaitingForMinecraft);
            wait_for(Duration::from_secs(600), || minecraft_window().is_some()).ok_or("Minecraft didn't start (10 min)")?;
        }
        play.set(PlayStep::WaitingForWorld);
        // "Minecraft* 26.3 - Singleplayer" in a world; the part after the dash is translated (Russian: "Одиночная игра")
        wait_for(Duration::from_secs(240), || minecraft_window().map(|t| t.contains(" - ")).unwrap_or(false))
            .ok_or("Minecraft didn't open the BlockBreach world")?;
        if !ron_running() {
            play.set(PlayStep::StartingRon);
            start_ron(&inst)?;
            play.set(PlayStep::WaitingForRon);
            wait_for(Duration::from_secs(300), || !window_titles(RON_EXE).is_empty()).ok_or("Ready or Not didn't start")?;
        }
        Ok(())
    };
    match run() {
        Ok(()) => play.set(PlayStep::Done),
        Err(e) => play.set(PlayStep::Failed(e)),
    }
}

fn wait_for(limit: Duration, mut cond: impl FnMut() -> bool) -> Option<()> {
    let t0 = Instant::now();
    while t0.elapsed() < limit {
        if cond() {
            return Some(());
        }
        std::thread::sleep(Duration::from_millis(700));
    }
    None
}

fn start_launcher(l: &Launcher) -> Result<(), String> {
    match l {
        Launcher::Store => Command::new("explorer.exe")
            .arg(format!("shell:AppsFolder\\{STORE_PACKAGE}!Minecraft"))
            .spawn()
            .map(|_| ())
            .map_err(|e| e.to_string()),
        Launcher::Exe(p) => Command::new(p).spawn().map(|_| ()).map_err(|e| e.to_string()),
    }
}

/// Through Steam (it must be running for RoN anyway), with -dx11: the compositor works on DirectX 11.
fn start_ron(inst: &Installed) -> Result<(), String> {
    if let Some(steam) = steam_dir() {
        let exe = steam.join("steam.exe");
        if exe.is_file() {
            return Command::new(exe).args(["-applaunch", RON_APP_ID, "-dx11"]).spawn().map(|_| ()).map_err(|e| e.to_string());
        }
    }
    let exe = inst.ron_win64.join(RON_EXE);
    Command::new(&exe).arg("-dx11").current_dir(&inst.ron_win64).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Restart this exe elevated (UAC), with the given arguments.
pub fn restart_as_admin(args: &str) -> bool {
    use windows_sys::Win32::UI::Shell::ShellExecuteW;
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;
    let Ok(me) = std::env::current_exe() else { return false };
    let w = |s: &str| s.encode_utf16().chain(std::iter::once(0)).collect::<Vec<u16>>();
    let (verb, file, params) = (w("runas"), w(&me.to_string_lossy()), w(args));
    let r = unsafe { ShellExecuteW(std::ptr::null_mut(), verb.as_ptr(), file.as_ptr(), params.as_ptr(), std::ptr::null(), SW_SHOWNORMAL) };
    r as isize > 32
}

pub fn open_folder(p: &Path) {
    let _ = Command::new("explorer.exe").arg(p).spawn();
}

pub fn system_language_ru() -> bool {
    let id = unsafe { windows_sys::Win32::Globalization::GetUserDefaultUILanguage() };
    (id & 0x3ff) == 0x19 // LANG_RUSSIAN
}

/// Whether the installed files are all where the install put them (else: offer a repair).
pub fn install_intact(inst: &Installed) -> bool {
    inst.ron_files.iter().all(|r| rel_path(&inst.ron_win64, r).exists())
        && inst.game_dir.join("mods").join("blockbreach-passthrough.jar").exists()
        && inst.mc_dir.join("versions").join(FABRIC_VERSION).join(format!("{FABRIC_VERSION}.json")).exists()
}
