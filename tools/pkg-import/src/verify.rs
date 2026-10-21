// SPDX-License-Identifier: GPL-2.0-or-later
//! Shared importer/startup verifier. No timestamp-only approval without a prior SHA256 pass.
use super::Progress;
use anyhow::{Context, Result, bail, ensure};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fs,
    io::Read,
    path::{Component, Path, PathBuf},
    time::{Instant, UNIX_EPOCH},
};

fn safe_relative(name: &str) -> bool {
    !name.is_empty()
        && !name.contains(['\\', ':'])
        && name
            .split('/')
            .all(|c| !c.is_empty() && c != "." && c != "..")
        && Path::new(name)
            .components()
            .all(|c| matches!(c, Component::Normal(_)))
}
fn fingerprint(m: &fs::Metadata) -> Value {
    let stamp = |t: std::io::Result<std::time::SystemTime>| {
        t.ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|t| json!([t.as_secs(), t.subsec_nanos()]))
    };
    #[allow(unused_mut)] // Unix additionally records inode/ctime; Windows uses creation/write time.
    let mut value =
        json!({"size":m.len(),"modified":stamp(m.modified()),"created":stamp(m.created())});
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        value["identity"] = json!([m.dev(), m.ino(), m.ctime(), m.ctime_nsec()]);
    }
    value
}
fn audit_hash(audit: &Value) -> String {
    format!("{:x}", Sha256::digest(audit.to_string().as_bytes()))
}
fn cache_path(root: &Path, directory: &Path) -> PathBuf {
    directory.join(format!(
        "{:x}.json",
        Sha256::digest(root.to_string_lossy().as_bytes())
    ))
}
fn path_key(name: &str) -> String {
    if cfg!(windows) {
        name.to_ascii_lowercase()
    } else {
        name.to_string()
    }
}
fn ignored(name: &str, audit: &Value) -> bool {
    audit["ignored_files"]
        .as_array()
        .into_iter()
        .flatten()
        .any(|v| v.as_str().is_some_and(|s| s.eq_ignore_ascii_case(name)))
        || audit["ignored_directories"]
            .as_array()
            .into_iter()
            .flatten()
            .any(|v| {
                v.as_str().is_some_and(|s| {
                    name.eq_ignore_ascii_case(s)
                        || name
                            .to_ascii_lowercase()
                            .starts_with(&(s.to_ascii_lowercase() + "/"))
                })
            })
}
fn scan(
    root: &Path,
    at: &Path,
    audit: &Value,
    expected: &BTreeSet<String>,
    issues: &mut Vec<String>,
    skipped: &mut Vec<String>,
) -> Result<()> {
    for entry in fs::read_dir(at).with_context(|| format!("无法读取目录：{}", at.display()))?
    {
        let entry = entry?;
        let path = entry.path();
        let name = path
            .strip_prefix(root)?
            .to_string_lossy()
            .replace('\\', "/");
        if ignored(&name, audit) {
            skipped.push(name);
            continue;
        }
        let kind = entry.file_type()?;
        if kind.is_symlink() {
            issues.push(format!("不允许链接文件或目录：{name}"));
        } else if kind.is_dir() {
            scan(root, &path, audit, expected, issues, skipped)?;
        } else if !kind.is_file() || !expected.contains(&path_key(&name)) {
            issues.push(format!("未识别的额外文件：{name}"));
        }
    }
    Ok(())
}

pub(super) struct Verified {
    entries: Value,
}
impl Verified {
    pub(super) fn save_cache(&self, root: &Path, audit: &Value, directory: &Path) -> Result<()> {
        let root = fs::canonicalize(root)?;
        fs::create_dir_all(directory)?;
        let path = cache_path(&root, directory);
        let temp = path.with_extension("tmp");
        let cache =
            json!({"schema":1,"root":root,"audit_sha256":audit_hash(audit),"files":self.entries});
        fs::write(&temp, serde_json::to_vec_pretty(&cache)?)?;
        fs::rename(&temp, &path).context("无法保存文件校验记录")?;
        Ok(())
    }
}
pub(super) fn verify(
    root: &Path,
    audit: &Value,
    progress: &Progress,
    cache_dir: Option<&Path>,
    force: bool,
) -> Result<Verified> {
    ensure!(audit["verification_schema"] == 1, "游戏校验清单版本不支持");
    let expected = audit["files"].as_object().context("缺少游戏文件哈希清单")?;
    ensure!(!expected.is_empty(), "游戏文件哈希清单为空");
    for (name, hash) in expected {
        ensure!(
            safe_relative(name)
                && hash
                    .as_str()
                    .is_some_and(|s| s.len() == 64 && s.bytes().all(|c| c.is_ascii_hexdigit()))
                && audit["file_sizes"][name].as_u64().is_some(),
            "游戏校验清单无效：{name}"
        );
    }
    for name in audit["ignored_files"]
        .as_array()
        .into_iter()
        .flatten()
        .chain(
            audit["ignored_directories"]
                .as_array()
                .into_iter()
                .flatten(),
        )
    {
        ensure!(
            name.as_str().is_some_and(safe_relative),
            "游戏校验排除项无效"
        );
    }
    let root = fs::canonicalize(root).context("游戏目录不存在或无法读取")?;
    let cache = if force {
        None
    } else {
        cache_dir
            .and_then(|dir| fs::read(cache_path(&root, dir)).ok())
            .and_then(|b| serde_json::from_slice::<Value>(&b).ok())
            .filter(|v| {
                v["schema"] == 1
                    && v["root"] == json!(root)
                    && v["audit_sha256"] == audit_hash(audit)
            })
    };
    let mode = if force {
        "full"
    } else if cache.is_some() {
        "incremental"
    } else {
        "first"
    };
    let total_bytes: u64 = expected
        .keys()
        .map(|n| audit["file_sizes"][n].as_u64().unwrap())
        .sum();
    progress.update(|s| {
        s["phase"] = json!("verify");
        s["verification_mode"] = json!(mode);
        s["done"] = json!(0);
        s["total"] = json!(expected.len());
        s["bytes"] = json!(0);
        s["total_bytes"] = json!(total_bytes);
        s["cached"] = json!(0);
        s["file"] = json!("");
    });
    let mut issues = Vec::new();
    let mut skipped = Vec::new();
    let names = expected.keys().map(|s| path_key(s)).collect();
    scan(&root, &root, audit, &names, &mut issues, &mut skipped)?;
    let mut records = Vec::new();
    let mut entries = json!({});
    let mut done = 0;
    let mut bytes = 0u64;
    let mut cached = 0;
    let mut hashed = 0;
    let mut block = vec![0u8; 1024 * 1024];
    let mut last_progress = Instant::now();
    for (name, hash) in expected {
        let size = audit["file_sizes"][name].as_u64().unwrap();
        progress.update(|s| s["file"] = json!(name));
        let mut record = json!({"file":name,"expected_sha256":hash,"expected_size":size});
        let result = (|| -> Result<()> {
            let path = root.join(name);
            let metadata = fs::symlink_metadata(&path)
                .with_context(|| format!("缺少或无法读取文件：{name}"))?;
            ensure!(
                metadata.is_file() && !metadata.file_type().is_symlink(),
                "不是普通文件：{name}"
            );
            record["actual_size"] = json!(metadata.len());
            ensure!(
                metadata.len() == size,
                "文件大小不一致：{name}（应为 {size}，实际 {} 字节）",
                metadata.len()
            );
            let stamp = fingerprint(&metadata);
            let previous = cache.as_ref().map(|c| &c["files"][name]);
            let reusable = previous.is_some_and(|p| {
                p["sha256"] == *hash && p["fingerprint"] == stamp && !stamp["modified"].is_null()
            });
            let actual = if reusable {
                cached += 1;
                bytes += size;
                hash.as_str().unwrap().to_string()
            } else {
                let mut input =
                    fs::File::open(&path).with_context(|| format!("无法读取文件：{name}"))?;
                let mut digest = Sha256::new();
                let mut read = 0u64;
                loop {
                    let n = match input.read(&mut block) {
                        Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                        v => v,
                    }
                    .with_context(|| format!("读取失败：{name}"))?;
                    if n == 0 {
                        break;
                    }
                    digest.update(&block[..n]);
                    bytes += n as u64;
                    read += n as u64;
                    if last_progress.elapsed().as_millis() >= 100 {
                        progress.update(|s| s["bytes"] = json!(bytes));
                        last_progress = Instant::now();
                    }
                }
                ensure!(
                    read == size && fingerprint(&fs::metadata(&path)?) == stamp,
                    "校验期间文件发生变化：{name}"
                );
                hashed += 1;
                format!("{:x}", digest.finalize())
            };
            record["actual_sha256"] = json!(actual);
            ensure!(actual == hash.as_str().unwrap(), "SHA256 不一致：{name}");
            record["status"] = json!(if reusable { "cached" } else { "verified" });
            entries[name] = json!({"sha256":hash,"fingerprint":stamp});
            Ok(())
        })();
        if let Err(error) = result {
            let error = format!("{error:#}");
            record["status"] = json!("error");
            record["error"] = json!(error);
            issues.push(error);
        }
        records.push(record);
        done += 1;
        progress.update(|s| {
            s["done"] = json!(done);
            s["bytes"] = json!(bytes);
            s["cached"] = json!(cached);
        });
    }
    skipped.sort();
    issues.sort();
    let report = json!({"schema":1,"root":root,"audit_sha256":audit_hash(audit),"mode":mode,"passed":issues.is_empty(),"total_files":expected.len(),"cached_files":cached,"hashed_files":hashed,"files":records,"ignored":skipped,"issues":issues});
    fs::write(
        progress.path.with_extension("json"),
        serde_json::to_vec_pretty(&report)?,
    )
    .context("无法保存文件校验报告")?;
    if !issues.is_empty() {
        let summary = format!(
            "游戏文件校验失败（{} 项）：{}{}。详细报告：{}",
            issues.len(),
            issues
                .iter()
                .take(3)
                .cloned()
                .collect::<Vec<_>>()
                .join("；"),
            if issues.len() > 3 {
                "；其余见报告"
            } else {
                ""
            },
            progress.path.with_extension("json").display()
        );
        bail!("{summary}");
    }
    progress.update(|s| s["phase"] = json!("verified"));
    eprintln!(
        "游戏文件校验通过：{} 个文件，{} 个复用校验记录，模式 {mode}",
        expected.len(),
        cached
    );
    Ok(Verified { entries })
}
