//! C-ABI bridge between the GNOME Software plugin (C) and the Modulix Rust
//! library (`modulix-core-utils`).
//!
//! Read-only on purpose: this layer only **searches** and **lists** nix
//! packages, Modulix modules and module plugins, and reads their metadata.
//! Every result is returned as a JSON string owned by the caller — free it with
//! [`backend_free_string`]. Install / remove is intentionally absent: the plugin
//! drives those over D-Bus to `org.modulix.Daemon`.
//!
//! JSON shapes:
//! - app list  → `[{ name, pname, summary, version, app_id?, icon?, kind,
//!                    flatpak_preferred, priority }, …]`  (`kind` ∈ package|module)
//! - metadata  → `{ description, version?, app_id?, icon?,
//!                   screenshots?: [{ caption, default, images: [{url,width,height}] }] }`
//! - enrich    → `{ description?, screenshots? }`  (Flathub-only, by app_id)
//! - plugins   → `[{ name, description }, …]`

use std::collections::{HashMap, HashSet};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_uint};
use std::ptr;
use std::sync::{Mutex, OnceLock};

use serde::Serialize;
use tokio::runtime::Runtime;

use modulix_core_utils::module_info::ModuleInfo;
use modulix_core_utils::mx::ErrorKind;
use modulix_core_utils::package_info::{self, NixPackage};
use modulix_core_utils::{
    AppInfoGui, AppInfoMinimal, AppScreenshot, CONFIG_DIRECTORY, FlatpakInfo, install_module,
    install_package,
};

pub type BackendResult = i32;
pub const BACKEND_OK: BackendResult = 0;
pub const BACKEND_ERROR: BackendResult = 1;

/// Source-priority hints (see the dedup/sources design in the plugin). A module
/// always outranks a nix package, which by default outranks a Flatpak; for apps
/// in [`package_info::FLATPAK_PREFERRED_APP_IDS`] the nix package is demoted
/// below the Flatpak (but never below a module).
const MODULE_PRIORITY: i32 = 1000;
const PACKAGE_PRIORITY: i32 = 500;
const PACKAGE_FLATPAK_PREFERRED_PRIORITY: i32 = 100;

// ─── Tokio runtime ──────────────────────────────────────────────────────────

static RUNTIME: OnceLock<Runtime> = OnceLock::new();

fn rt() -> &'static Runtime {
    RUNTIME.get_or_init(|| Runtime::new().expect("failed to build Tokio runtime"))
}

#[unsafe(no_mangle)]
pub extern "C" fn backend_init() -> BackendResult {
    rt();
    BACKEND_OK
}

#[unsafe(no_mangle)]
pub extern "C" fn backend_shutdown() {
    // The runtime lives for the whole process; nothing to tear down.
}

// ─── Memory / string helpers ────────────────────────────────────────────────

fn to_cstring(s: String) -> *mut c_char {
    CString::new(s)
        .map(CString::into_raw)
        .unwrap_or(ptr::null_mut())
}

fn cstr_to_str<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    // SAFETY: caller guarantees `ptr` is a valid NUL-terminated C string.
    unsafe { CStr::from_ptr(ptr).to_str().ok() }
}

fn json_to_cstring<T: Serialize>(value: &T) -> *mut c_char {
    match serde_json::to_string(value) {
        Ok(json) => to_cstring(json),
        Err(e) => {
            eprintln!("[backend] serialize: {e}");
            ptr::null_mut()
        }
    }
}

/// Free a string previously returned by any `backend_*` function.
///
/// # Safety
/// `ptr` must be either NULL or a pointer obtained from a `backend_*` function
/// in this library and not yet freed. Passing any other pointer, or freeing the
/// same pointer twice, is undefined behaviour.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn backend_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        // SAFETY: `ptr` was produced by `CString::into_raw` in this module.
        unsafe { drop(CString::from_raw(ptr)) };
    }
}

// ─── JSON payloads ──────────────────────────────────────────────────────────

#[derive(Serialize)]
struct AppEntry {
    /// nixpkgs attribute (package) or module name — the install identifier.
    /// For a non-default output row this is `<attr>.<output>` (e.g. `htop.man`).
    name: String,
    /// Bare package attribute, without any output suffix. Equals `name` for
    /// normal rows; the plugin uses it for the themed-icon lookup so an output
    /// row (`htop.man`) still resolves the base package icon (`htop`).
    base_name: String,
    /// Human display name (pname / module display name).
    pname: String,
    /// Flatpak/AppStream display name (e.g. "Firefox" for `firefox-bin`), when
    /// the package matches a known app. The plugin shows "<app_name> (<pname>)".
    #[serde(skip_serializing_if = "Option::is_none")]
    app_name: Option<String>,
    summary: String,
    version: String,
    /// Canonical AppStream id used to deduplicate against Flatpak/AppStream.
    #[serde(skip_serializing_if = "Option::is_none")]
    app_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    icon: Option<String>,
    kind: &'static str,
    flatpak_preferred: bool,
    priority: i32,
}

#[derive(Serialize)]
struct MetaEntry {
    description: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    version: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    app_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    icon: Option<String>,
    /// AppStream/Flathub screenshots, default one first-marked.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    screenshots: Vec<ShotEntry>,
}

#[derive(Serialize)]
struct ShotEntry {
    caption: String,
    default: bool,
    images: Vec<ImageEntry>,
}

#[derive(Serialize)]
struct ImageEntry {
    url: String,
    width: u32,
    height: u32,
}

fn package_entry(pkg: &NixPackage) -> AppEntry {
    let app_id = pkg.id().map(str::to_string);
    let flatpak_preferred = app_id
        .as_deref()
        .map(package_info::is_flatpak_preferred)
        .unwrap_or(false);
    AppEntry {
        name: pkg.package_name().to_string(),
        base_name: pkg.package_name().to_string(),
        pname: pkg.display_name().to_string(),
        app_name: pkg.app_name().map(str::to_string),
        summary: pkg.summary().to_string(),
        version: pkg.version().to_string(),
        app_id,
        icon: pkg.icon().map(str::to_string),
        kind: "package",
        flatpak_preferred,
        priority: if flatpak_preferred {
            PACKAGE_FLATPAK_PREFERRED_PRIORITY
        } else {
            PACKAGE_PRIORITY
        },
    }
}

/// Build a module entry. Call after `resolve()` so `icon()` is populated.
fn module_entry(module: &ModuleInfo) -> AppEntry {
    AppEntry {
        name: module.package_name().to_string(),
        base_name: module.package_name().to_string(),
        pname: module.display_name().to_string(),
        app_name: None,
        summary: module.summary().to_string(),
        version: String::new(),
        app_id: module.id().map(str::to_string),
        icon: module.icon().map(str::to_string),
        kind: "module",
        flatpak_preferred: false,
        priority: MODULE_PRIORITY,
    }
}

/// Convert the crate's borrowed [`AppScreenshot`] into owned JSON entries.
fn collect_screenshots(shot: Option<AppScreenshot<'_>>) -> Vec<ShotEntry> {
    let Some(shot) = shot else {
        return Vec::new();
    };
    shot.screenshots
        .iter()
        .enumerate()
        .map(|(i, sized)| ShotEntry {
            caption: sized.caption.to_string(),
            default: i == shot.default,
            images: sized
                .screenshot
                .iter()
                .map(|img| ImageEntry {
                    url: img.url.to_string(),
                    width: img.width,
                    height: img.height,
                })
                .collect(),
        })
        .collect()
}

/// Collapse nix variants that share a canonical app-id into one entry (e.g.
/// `firefox`, `firefox-bin` → a single "Firefox" row). The first entry per id
/// wins (search results are relevance-sorted); entries without an id are kept
/// individually. The dropped variants stay reachable through the alternate-of
/// query ([`backend_packages_for_app_id`]).
fn dedup_by_app_id(entries: Vec<AppEntry>) -> Vec<AppEntry> {
    let mut seen: HashSet<String> = HashSet::new();
    entries
        .into_iter()
        .filter(|e| match &e.app_id {
            Some(id) => seen.insert(id.clone()),
            None => true,
        })
        .collect()
}

// ─── Multi-output expansion ───────────────────────────────────────────────────

/// The default nix output: installing the bare attribute yields it.
const DEFAULT_OUTPUT: &str = "out";

/// Fetch the output names of every given nixpkgs attribute in a **single** `nix
/// eval` (avoid one process per package). Returns `attr → [outputs]`. On any
/// failure (no `nix`, eval error, parse error) returns an empty map, so callers
/// degrade gracefully to one row per package.
fn fetch_outputs(attrs: &[&str]) -> HashMap<String, Vec<String>> {
    if attrs.is_empty() {
        return HashMap::new();
    }
    // `{:?}` quotes/escapes the attr as a valid nix string for our (simple) attrs.
    let names = attrs
        .iter()
        .map(|a| format!("{a:?}"))
        .collect::<Vec<_>>()
        .join(" ");
    let expr = format!(
        r#"let f = builtins.getFlake "nixpkgs";
              pkgs = f.legacyPackages.${{builtins.currentSystem}};
              lib = f.lib;
              get = n:
                let r = builtins.tryEval
                          ((lib.attrByPath (lib.splitString "." n) null pkgs).outputs or []);
                in if r.success && r.value != null then r.value else [];
          in builtins.listToAttrs (map (n: {{ name = n; value = get n; }}) [ {names} ])"#
    );

    let output = std::process::Command::new("nix")
        .args(["eval", "--impure", "--json", "--expr", &expr])
        .output();
    match output {
        Ok(o) if o.status.success() => serde_json::from_slice(&o.stdout).unwrap_or_default(),
        Ok(o) => {
            eprintln!(
                "[backend] fetch_outputs: nix eval failed: {}",
                String::from_utf8_lossy(&o.stderr)
            );
            HashMap::new()
        }
        Err(e) => {
            eprintln!("[backend] fetch_outputs: {e}");
            HashMap::new()
        }
    }
}

/// Expand package entries with >1 output into one row per output: the default
/// output keeps the original entry (id, dedup, naming) untouched; every other
/// output becomes its own installable row `<attr>.<output>` displayed as
/// `<display>[<output>]`, with no `app_id` so it gets a unique GsApp id and does
/// not stack with the base/Flatpak app.
fn expand_with_outputs(
    entries: Vec<AppEntry>,
    outputs: &HashMap<String, Vec<String>>,
) -> Vec<AppEntry> {
    let mut result = Vec::with_capacity(entries.len());
    for entry in entries {
        let outs = outputs.get(&entry.name);
        let multi = entry.kind == "package" && outs.is_some_and(|o| o.len() > 1);
        if !multi {
            result.push(entry);
            continue;
        }
        let outs = outs.unwrap();
        let default = if outs.iter().any(|o| o == DEFAULT_OUTPUT) {
            DEFAULT_OUTPUT
        } else {
            outs[0].as_str()
        };

        let attr = entry.name.clone();
        let display = entry.pname.clone();
        let summary = entry.summary.clone();
        let version = entry.version.clone();
        let icon = entry.icon.clone();
        let flatpak_preferred = entry.flatpak_preferred;
        let priority = entry.priority;

        result.push(entry);
        for out in outs.iter().filter(|o| o.as_str() != default) {
            result.push(AppEntry {
                name: format!("{attr}.{out}"),
                base_name: attr.clone(),
                pname: format!("{display}[{out}]"),
                app_name: None,
                summary: summary.clone(),
                version: version.clone(),
                app_id: None,
                icon: icon.clone(),
                kind: "package",
                flatpak_preferred,
                priority,
            });
        }
    }
    result
}

/// Fetch outputs for all package entries then expand them (see
/// [`expand_with_outputs`]).
fn expand_outputs(entries: Vec<AppEntry>) -> Vec<AppEntry> {
    let attrs: Vec<&str> = entries
        .iter()
        .filter(|e| e.kind == "package")
        .map(|e| e.name.as_str())
        .collect();
    let outputs = fetch_outputs(&attrs);
    expand_with_outputs(entries, &outputs)
}

// ─── Search ─────────────────────────────────────────────────────────────────

/// Search nix packages. Returns a JSON app list.
#[unsafe(no_mangle)]
pub extern "C" fn backend_search_packages(query: *const c_char, number: c_uint) -> *mut c_char {
    let Some(query) = cstr_to_str(query) else {
        return ptr::null_mut();
    };
    match rt().block_on(NixPackage::search(query, number)) {
        Ok(pkgs) => {
            let entries = dedup_by_app_id(pkgs.iter().map(package_entry).collect());
            let entries = expand_outputs(entries);
            json_to_cstring(&entries)
        }
        Err(e) => {
            eprintln!("[backend] search_packages: {e}");
            ptr::null_mut()
        }
    }
}

/// Search Modulix modules. Returns a JSON app list (kind = "module").
#[unsafe(no_mangle)]
pub extern "C" fn backend_search_modules(query: *const c_char, number: c_uint) -> *mut c_char {
    let Some(query) = cstr_to_str(query) else {
        return ptr::null_mut();
    };
    let result = rt().block_on(async {
        let modules = ModuleInfo::search(query, number).await?;
        for module in &modules {
            module.resolve().await;
        }
        Ok::<_, ErrorKind>(modules)
    });
    match result {
        Ok(modules) => {
            let entries: Vec<AppEntry> = modules.iter().map(module_entry).collect();
            json_to_cstring(&entries)
        }
        Err(e) => {
            eprintln!("[backend] search_modules: {e}");
            ptr::null_mut()
        }
    }
}

// ─── Installed listings ─────────────────────────────────────────────────────

/// List installed nix packages (`environment.systemPackages`). JSON app list.
#[unsafe(no_mangle)]
pub extern "C" fn backend_list_installed_packages() -> *mut c_char {
    match install_package::list_installed_package(CONFIG_DIRECTORY) {
        Ok(pkgs) => {
            let entries = dedup_by_app_id(pkgs.iter().map(package_entry).collect());
            let entries = expand_outputs(entries);
            json_to_cstring(&entries)
        }
        Err(e) => {
            eprintln!("[backend] list_installed_packages: {e}");
            ptr::null_mut()
        }
    }
}

/// List installed Modulix modules (`mx.*.enable`). JSON app list (kind = "module").
#[unsafe(no_mangle)]
pub extern "C" fn backend_list_installed_modules() -> *mut c_char {
    let result = rt().block_on(async {
        let modules = install_module::list_installed_modules(CONFIG_DIRECTORY).await?;
        for module in &modules {
            module.resolve().await;
        }
        Ok::<_, ErrorKind>(modules)
    });
    match result {
        Ok(modules) => {
            let entries: Vec<AppEntry> = modules.iter().map(module_entry).collect();
            json_to_cstring(&entries)
        }
        Err(e) => {
            eprintln!("[backend] list_installed_modules: {e}");
            ptr::null_mut()
        }
    }
}

// ─── Metadata ───────────────────────────────────────────────────────────────

/// Rich metadata for one nix package. JSON metadata object.
#[unsafe(no_mangle)]
pub extern "C" fn backend_get_package_metadata(name: *const c_char) -> *mut c_char {
    let Some(name) = cstr_to_str(name) else {
        return ptr::null_mut();
    };
    let result = rt().block_on(async {
        let pkg = NixPackage::new(name).await?;
        let description = pkg.description().await.into_owned();
        let version = pkg.version().to_string();
        let screenshots = collect_screenshots(pkg.screenshots().await);
        Ok::<_, ErrorKind>(MetaEntry {
            description,
            version: (!version.is_empty()).then_some(version),
            app_id: pkg.id().map(str::to_string),
            icon: pkg.icon().map(str::to_string),
            screenshots,
        })
    });
    match result {
        Ok(meta) => json_to_cstring(&meta),
        Err(e) => {
            eprintln!("[backend] get_package_metadata: {e}");
            ptr::null_mut()
        }
    }
}

/// Rich metadata for one module. JSON metadata object.
#[unsafe(no_mangle)]
pub extern "C" fn backend_get_module_metadata(name: *const c_char) -> *mut c_char {
    let Some(name) = cstr_to_str(name) else {
        return ptr::null_mut();
    };
    let result = rt().block_on(async {
        let module = ModuleInfo::new(name).await?;
        let description = module.description().await.into_owned();
        let screenshots = collect_screenshots(module.screenshots().await);
        Ok::<_, ErrorKind>(MetaEntry {
            description,
            version: None,
            app_id: module.id().map(str::to_string),
            icon: module.icon().map(str::to_string),
            screenshots,
        })
    });
    match result {
        Ok(meta) => json_to_cstring(&meta),
        Err(e) => {
            eprintln!("[backend] get_module_metadata: {e}");
            ptr::null_mut()
        }
    }
}

// ─── Module plugins (addons) ────────────────────────────────────────────────

/// List the plugins (addons) of a module. JSON: `[{ name, description }, …]`.
#[unsafe(no_mangle)]
pub extern "C" fn backend_list_module_plugins(name: *const c_char) -> *mut c_char {
    let Some(name) = cstr_to_str(name) else {
        return ptr::null_mut();
    };
    let result = rt().block_on(async {
        let module = ModuleInfo::new(name).await?;
        module.list_plugins().await
    });
    match result {
        Ok(plugins) => json_to_cstring(&plugins),
        Err(e) => {
            eprintln!("[backend] list_module_plugins: {e}");
            ptr::null_mut()
        }
    }
}

// ─── Flathub enrichment (refine) ──────────────────────────────────────────────

#[derive(Serialize)]
struct EnrichEntry {
    #[serde(skip_serializing_if = "Option::is_none")]
    description: Option<String>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    screenshots: Vec<ShotEntry>,
}

/// Per-app_id cache of the enrichment JSON, so refining a whole list hits Flathub
/// at most once per application.
static ENRICH_CACHE: OnceLock<Mutex<HashMap<String, Option<String>>>> = OnceLock::new();

fn enrich_cache() -> &'static Mutex<HashMap<String, Option<String>>> {
    ENRICH_CACHE.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Rich Flathub/AppStream metadata (long description + screenshots) for a
/// canonical app-id. Pure HTTP — no `nix` involved — so it works inside the
/// GNOME Software sandbox where `<nixpkgs>` is unavailable. Returns JSON
/// `{ description?, screenshots? }`, or NULL when the app is not on Flathub /
/// offline. Results are cached by app-id.
#[unsafe(no_mangle)]
pub extern "C" fn backend_get_app_enrichment(app_id: *const c_char) -> *mut c_char {
    let Some(app_id) = cstr_to_str(app_id) else {
        return ptr::null_mut();
    };

    if let Some(cached) = enrich_cache().lock().unwrap().get(app_id) {
        return match cached {
            Some(json) => to_cstring(json.clone()),
            None => ptr::null_mut(),
        };
    }

    let json = rt().block_on(async {
        let info = FlatpakInfo::new(app_id).await.ok()?;
        let description = info.description();
        let entry = EnrichEntry {
            description: (!description.is_empty()).then(|| description.to_string()),
            screenshots: collect_screenshots(info.screenshots()),
        };
        serde_json::to_string(&entry).ok()
    });

    enrich_cache()
        .lock()
        .unwrap()
        .insert(app_id.to_string(), json.clone());

    match json {
        Some(json) => to_cstring(json),
        None => ptr::null_mut(),
    }
}

// ─── Alternate sources ──────────────────────────────────────────────────────

/// All nix package variants of a canonical app-id, for GNOME Software's
/// `alternate-of` query. Cheap (table lookup, no nix eval); the plugin refines
/// each entry afterwards. JSON app list.
#[unsafe(no_mangle)]
pub extern "C" fn backend_packages_for_app_id(app_id: *const c_char) -> *mut c_char {
    let Some(app_id) = cstr_to_str(app_id) else {
        return ptr::null_mut();
    };
    let flatpak_preferred = package_info::is_flatpak_preferred(app_id);
    let priority = if flatpak_preferred {
        PACKAGE_FLATPAK_PREFERRED_PRIORITY
    } else {
        PACKAGE_PRIORITY
    };
    let entries: Vec<AppEntry> = package_info::packages_for_app_id(app_id)
        .into_iter()
        .map(|attr| AppEntry {
            name: attr.to_string(),
            base_name: attr.to_string(),
            pname: attr.to_string(),
            app_name: package_info::name_for_package(attr).map(str::to_string),
            summary: String::new(),
            version: String::new(),
            app_id: Some(app_id.to_string()),
            icon: None,
            kind: "package",
            flatpak_preferred,
            priority,
        })
        .collect();
    json_to_cstring(&entries)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn init_and_free_null() {
        assert_eq!(backend_init(), BACKEND_OK);
        // freeing NULL is a no-op
        unsafe { backend_free_string(ptr::null_mut()) };
        backend_shutdown();
    }

    fn pkg_entry(name: &str, app_id: Option<&str>) -> AppEntry {
        AppEntry {
            name: name.to_string(),
            base_name: name.to_string(),
            pname: name.to_string(),
            app_name: None,
            summary: "summary".to_string(),
            version: "1.0".to_string(),
            app_id: app_id.map(str::to_string),
            icon: None,
            kind: "package",
            flatpak_preferred: false,
            priority: PACKAGE_PRIORITY,
        }
    }

    #[test]
    fn expand_outputs_single_output_unchanged() {
        let mut map = HashMap::new();
        map.insert("hello".to_string(), vec!["out".to_string()]);
        let out = expand_with_outputs(vec![pkg_entry("hello", None)], &map);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].name, "hello");
    }

    #[test]
    fn expand_outputs_no_data_unchanged() {
        let out = expand_with_outputs(vec![pkg_entry("hello", None)], &HashMap::new());
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].name, "hello");
    }

    #[test]
    fn expand_outputs_splits_non_default() {
        let mut map = HashMap::new();
        map.insert(
            "htop".to_string(),
            vec!["out".to_string(), "man".to_string(), "dev".to_string()],
        );
        let out = expand_with_outputs(vec![pkg_entry("htop", Some("org.htop.htop"))], &map);
        assert_eq!(out.len(), 3);

        // Default output: original entry kept verbatim (keeps its app_id / dedup).
        assert_eq!(out[0].name, "htop");
        assert_eq!(out[0].app_id.as_deref(), Some("org.htop.htop"));

        let man = out.iter().find(|e| e.name == "htop.man").expect("man row");
        assert_eq!(man.pname, "htop[man]");
        assert_eq!(man.base_name, "htop");
        assert!(man.app_id.is_none(), "output row must have a unique id");
        assert_eq!(man.summary, "summary");

        assert!(out.iter().any(|e| e.name == "htop.dev"));
        assert!(out.iter().all(|e| e.name != "htop.out"));
    }

    #[test]
    fn expand_outputs_no_out_uses_first_as_default() {
        let mut map = HashMap::new();
        map.insert(
            "foo".to_string(),
            vec!["bin".to_string(), "lib".to_string()],
        );
        let out = expand_with_outputs(vec![pkg_entry("foo", None)], &map);
        assert_eq!(out.len(), 2);
        assert_eq!(out[0].name, "foo"); // first output is the default → base row
        assert!(out.iter().any(|e| e.name == "foo.lib"));
        assert!(out.iter().all(|e| e.name != "foo.bin"));
    }

    #[test]
    fn alternate_sources_serialize() {
        let id = CString::new("org.mozilla.firefox").unwrap();
        let ptr = backend_packages_for_app_id(id.as_ptr());
        if !ptr.is_null() {
            unsafe { backend_free_string(ptr) };
        }
    }
}
