// SPDX-License-Identifier: GPL-2.0-or-later
use anyhow::{Context, Result, ensure};
use orbis_pkg_util::{ExtractProgress, PkgExtractor, open_pkg};
use serde_json::json;
use std::{
    fs,
    path::{Path, PathBuf},
    sync::Mutex,
};

mod verify;

struct Progress {
    path: PathBuf,
    state: Mutex<serde_json::Value>,
}
impl Progress {
    fn update(&self, change: impl FnOnce(&mut serde_json::Value)) {
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        change(&mut state);
        // A reader may briefly see an incomplete snapshot; it should retry next frame.
        let text = state
            .as_object()
            .unwrap()
            .iter()
            .map(|(k, v)| {
                format!(
                    "{}={}\n",
                    k,
                    v.as_str()
                        .map(str::to_string)
                        .unwrap_or_else(|| v.to_string())
                        .replace(['\n', '\r'], " ")
                )
            })
            .collect::<String>();
        let _ = fs::write(&self.path, text);
    }
}
impl ExtractProgress for &Progress {
    fn entry_start(&self, path: &Path, _current: usize, _total: usize) {
        self.update(|s| {
            s["phase"] = json!("metadata");
            s["file"] = json!(path.file_name().unwrap_or_default().to_string_lossy());
        });
    }
    fn pfs_start(&self, total: usize) {
        self.update(|s| {
            s["phase"] = json!("extract");
            s["total"] = json!(total);
            s["done"] = json!(0);
            s["bytes"] = json!(0);
        });
    }
    fn pfs_file(&self, path: &Path, _size: u64) {
        eprintln!("Extract: {} ({} bytes)", path.display(), _size);
        self.update(|s| s["file"] = json!(path.file_name().unwrap_or_default().to_string_lossy()));
    }
    fn pfs_file_completed(&self, written: u64) {
        self.update(|s| {
            s["done"] = json!(s["done"].as_u64().unwrap_or(0) + 1);
            s["bytes"] = json!(s["bytes"].as_u64().unwrap_or(0) + written);
        });
    }
}
fn extract(
    pkg_path: &Path,
    destination: &Path,
    stage: &Path,
    profile: &Path,
    progress: &Progress,
    cache_dir: Option<&Path>,
) -> Result<()> {
    ensure!(
        !destination.exists(),
        "目标目录已存在，请选择一个新的目录；导入不会覆盖现有游戏。"
    );
    ensure!(!stage.exists(), "临时目录已存在，请重新开始导入。");
    let audit: serde_json::Value = serde_json::from_slice(&fs::read(profile)?)?;
    progress.update(|s| s["phase"] = json!("open"));
    let pkg = unsafe { open_pkg(pkg_path) }
        .context("无法读取或解密该 PKG；请使用可解包的 CUSA18093 本体包")?;
    let title = pkg.header().content_id().title_id();
    ensure!(
        title == audit["title_id"].as_str().unwrap_or(""),
        "游戏编号不支持：{title}，当前仅支持 CUSA18093。"
    );
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::create_dir(stage)?;
    PkgExtractor::new(&pkg, progress, false)
        .extract(stage)
        .context("解包失败，请检查目标盘空间、写入权限及文件系统限制；原有目录保持不变")?;
    let checked = verify::verify(stage, &audit, progress, None, true)?;
    ensure!(!destination.exists(), "目标目录已存在，导入已停止。");
    fs::rename(stage, destination).context("无法将解包结果移到目标目录")?;
    // The final directory has been committed; diagnostic update failures must not undo import.
    let update_report = (|| -> Result<()> {
        let report_path = progress.path.with_extension("json");
        let mut report: serde_json::Value = serde_json::from_slice(&fs::read(&report_path)?)?;
        report["root"] = json!(fs::canonicalize(destination)?);
        fs::write(&report_path, serde_json::to_vec_pretty(&report)?)?;
        Ok(())
    })();
    if let Err(error) = update_report {
        eprintln!("无法更新导入报告目录：{error:#}");
    }
    if let Some(directory) = cache_dir {
        if let Err(error) = checked.save_cache(destination, &audit, directory) {
            eprintln!("校验记录未保存，下次启动会重新完整校验：{error:#}");
        }
    }
    progress.update(|s| {
        s["phase"] = json!("complete");
        s["folder"] = json!(destination);
    });
    Ok(())
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    let check = args
        .get(1)
        .is_some_and(|v| v == "--verify" || v == "--verify-full");
    if check {
        if args.len() != 5 && args.len() != 6 {
            eprintln!(
                "Usage: aegis-pkg --verify[-full] GAME PROGRESS_INI PROFILE_JSON [CACHE_DIRECTORY]"
            );
            std::process::exit(2);
        }
        let progress = Progress {
            path: args[3].clone().into(),
            state: Mutex::new(json!({"phase":"verify"})),
        };
        let directory = args.get(5).map(Path::new);
        let result = (|| -> Result<()> {
            let audit: serde_json::Value = serde_json::from_slice(&fs::read(&args[4])?)?;
            let checked = verify::verify(
                Path::new(&args[2]),
                &audit,
                &progress,
                directory,
                args[1] == "--verify-full",
            )?;
            if let Some(dir) = directory {
                if let Err(error) = checked.save_cache(Path::new(&args[2]), &audit, dir) {
                    eprintln!("校验记录未保存，下次启动会重新完整校验：{error:#}");
                }
            }
            Ok(())
        })();
        if let Err(error) = result {
            let message = format!("{error:#}");
            progress.update(|s| {
                s["phase"] = json!("error");
                s["error"] = json!(message);
            });
            eprintln!("{message}");
            std::process::exit(1);
        }
        return;
    }
    if args.len() != 6 && args.len() != 7 {
        eprintln!(
            "Usage: aegis-pkg PKG DESTINATION STAGING PROGRESS_INI PROFILE_JSON [CACHE_DIRECTORY]"
        );
        std::process::exit(2);
    }
    let pkg = Path::new(&args[1]);
    let destination = Path::new(&args[2]);
    let stage = Path::new(&args[3]);
    let progress = Progress {
        path: args[4].clone().into(),
        state: Mutex::new(json!({"phase":"open","done":0,"total":0,"bytes":0})),
    };
    if stage.parent() != destination.parent()
        || !stage
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .starts_with(".aegis-import-")
        || stage.exists()
    {
        eprintln!("Invalid or existing staging directory");
        std::process::exit(2);
    }
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        extract(
            pkg,
            destination,
            stage,
            Path::new(&args[5]),
            &progress,
            args.get(6).map(Path::new),
        )
    }));
    let error = match result {
        Ok(Ok(())) => return,
        Ok(Err(e)) => format!("{e:#}"),
        Err(_) => "PKG 数据损坏或格式不支持，解包器已停止。".to_string(),
    };
    let _ = fs::remove_dir_all(stage);
    progress.update(|s| {
        s["phase"] = json!("error");
        s["error"] = json!(error);
    });
    eprintln!("{error}");
    std::process::exit(1);
}
