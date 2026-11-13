#!/usr/bin/env python3
import sys
import os
import json
import zipfile
import argparse
import re
import subprocess
from pathlib import Path
from typing import Optional

# Switch to project root directory
os.chdir(Path(__file__).resolve().parent.parent)

################################################################################
# Common utility functions
################################################################################

def get_board_type_from_compile_commands() -> Optional[str]:
    """Parse the current compiled BOARD_TYPE from build/compile_commands.json"""
    compile_file = Path("build/compile_commands.json")
    if not compile_file.exists():
        return None
    with compile_file.open(encoding='utf-8') as f:
        data = json.load(f)
    for item in data:
        if not item["file"].endswith("main.cc"):
            continue
        cmd = item["command"]
        if "-DBOARD_TYPE=\\\"" in cmd:
            return cmd.split("-DBOARD_TYPE=\\\"")[1].split("\\\"")[0].strip()
    return None


def get_project_version() -> Optional[str]:
    """Read set(PROJECT_VER "x.y.z") from root CMakeLists.txt"""
    with Path("CMakeLists.txt").open(encoding='utf-8') as f:
        for line in f:
            if line.startswith("set(PROJECT_VER"):
                return line.split("\"")[1]
    return None


def merge_bin() -> bool:
    if os.system("idf.py merge-bin") != 0:
        print("merge-bin failed", file=sys.stderr)
        return False
    return True


def zip_bin(name: str, version: str, out_dir: Optional[str] = None) -> None:
    """Zip build/merged-binary.bin to <out_dir>/v{version}_{name}.zip

    If out_dir is None, defaults to "releases".
    """
    base = Path(out_dir) if out_dir else Path("releases")
    base.mkdir(parents=True, exist_ok=True)
    output_path = base / f"v{version}_{name}.zip"

    if output_path.exists():
        output_path.unlink()

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED) as zipf:
        zipf.write("build/merged-binary.bin", arcname="merged-binary.bin")
    print(f"zip bin to {output_path} done")


_SUMMARY_LOG = Path("build_summary.log")

# Global progress counters for the summary log. _SUMMARY_TOTAL is set once in
# the CLI to the total number of variants to build; _SUMMARY_INDEX increments
# on every variant outcome so the summary shows e.g. [1/170] [OK] name.
_SUMMARY_TOTAL = 0
_SUMMARY_INDEX = 0


def _set_summary_total(total: int) -> None:
    """Record the total number of variants to build (for progress display)."""
    global _SUMMARY_TOTAL
    _SUMMARY_TOTAL = total


def _write_summary(status: str, final_name: str, detail: str = "") -> None:
    """Append a one-line build result to build_summary.log.

    Keeps the summary log tiny (one line per variant) and real-time, so it can
    be tailed while the batch build is still running. The full verbose log
    (build.log) is written separately by build_all.sh.
    """
    global _SUMMARY_INDEX
    if status in ("OK", "FAIL", "SKIP"):
        _SUMMARY_INDEX += 1
    progress = f"[{_SUMMARY_INDEX}/{_SUMMARY_TOTAL}] " if _SUMMARY_TOTAL else ""
    line = f"{progress}[{status}] {final_name}"
    if detail:
        line += f" ({detail})"
    with _SUMMARY_LOG.open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def _get_manufacturer(cfg: dict) -> Optional[str]:
    """Read manufacturer from config.json"""
    m = cfg.get("manufacturer")
    if isinstance(m, str) and m.strip():
        return m.strip()
    return None


def _parse_version(value: str) -> tuple[int, int, int]:
    """Parse an ESP-IDF version string such as 5.5.4 or v6.0."""
    match = re.search(r"v?(\d+)\.(\d+)(?:\.(\d+))?", value)
    if not match:
        raise ValueError(f"Invalid ESP-IDF version: {value}")
    return tuple(int(part or 0) for part in match.groups())


def _detect_idf_version() -> tuple[int, int, int]:
    """Resolve the active ESP-IDF version for version-gated build variants."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        version_file = Path(idf_path) / "tools/cmake/version.cmake"
        if version_file.exists():
            values: dict[str, int] = {}
            for line in version_file.read_text(encoding="utf-8").splitlines():
                match = re.match(
                    r"set\(IDF_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)\)", line
                )
                if match:
                    values[match.group(1)] = int(match.group(2))
            if all(part in values for part in ("MAJOR", "MINOR", "PATCH")):
                return values["MAJOR"], values["MINOR"], values["PATCH"]

    try:
        output = subprocess.run(
            ["idf.py", "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        return _parse_version(output)
    except (FileNotFoundError, subprocess.CalledProcessError, ValueError) as error:
        raise RuntimeError(
            "ESP-IDF version was not detected. "
            "Source export.sh before running build_all.py."
        ) from error


def _version_matches(version: tuple[int, int, int], expression: str) -> bool:
    """Evaluate a single comparison such as '<6.0' or '>=6.0.1'."""
    match = re.fullmatch(
        r"\s*(<=|>=|<|>|==)\s*(v?\d+\.\d+(?:\.\d+)?)\s*", expression
    )
    if not match:
        raise ValueError(f"Invalid ESP-IDF version expression: {expression}")
    operator, expected_text = match.groups()
    expected = _parse_version(expected_text)
    return {
        "<": version < expected,
        "<=": version <= expected,
        ">": version > expected,
        ">=": version >= expected,
        "==": version == expected,
    }[operator]


def _get_builds_for_idf(cfg: dict,
                        idf_version: tuple[int, int, int]) -> list[dict]:
    """Return build entries whose optional ESP-IDF version rule matches."""
    builds: list[dict] = []
    for build in cfg.get("builds", []):
        expression = build.get("idf_version")
        if expression and not _version_matches(idf_version, expression):
            continue
        builds.append(dict(build))
    return builds


################################################################################
# board / variant related functions
################################################################################

_BOARDS_DIR = Path("main/boards")

_REPORTED_IDENTIFIER_PATTERN = re.compile(r"^[a-z0-9.-]+$")


def _validate_reported_identifier(value: object, field: str) -> str:
    """Validate an OTA-reported board type or name (lowercase letters/digits/.-)."""
    if not isinstance(value, str) or not value:
        raise ValueError(f"missing non-empty {field}")
    if not _REPORTED_IDENTIFIER_PATTERN.fullmatch(value):
        raise ValueError(
            f"{field} {value!r} must contain only lowercase letters, "
            'digits, "." and "-"'
        )
    return value


def _collect_variants(config_filename: str = "config.json") -> list[dict[str, str]]:
    """Traverse all boards under main/boards, collect and validate variants.

    Mirrors scripts/build.py._collect_variants: enforces a unique OTA-reported
    type/name/identity per board and a unique artifact name, so release zips
    never silently overwrite each other.

    Return example:
        [{"board": "bread-compact-ml307", "name": "bread-compact-ml307", "full_name": "bread-compact-ml307"}, ...]
        [{"board": "waveshare/esp32-p4-nano", "name": "esp32-p4-nano-10.1-a", "full_name": "waveshare-esp32-p4-nano-10.1-a"}, ...]
    """
    variants: list[dict[str, str]] = []
    errors: list[str] = []
    type_owners: dict[str, str] = {}
    name_owners: dict[str, str] = {}

    for cfg_path in _BOARDS_DIR.rglob(config_filename):
        board_dir = cfg_path.parent
        if board_dir.name == "common":
            continue
        board = board_dir.relative_to(_BOARDS_DIR).as_posix()

        try:
            with cfg_path.open(encoding='utf-8') as f:
                cfg = json.load(f)

            manufacturer = _get_manufacturer(cfg)
            # `type` is the OTA-reported identity used by build.py; it is not
            # required for build_all.py (which names zips by manufacturer+name).
            # Validate it only when present.
            reported_type: Optional[str] = None
            raw_type = cfg.get("type")
            if raw_type is not None:
                reported_type = _validate_reported_identifier(
                    raw_type, 'top-level "type"'
                )
                # Unique reported board type (informational for build_all.py).
                previous = type_owners.get(reported_type)
                if previous is not None and previous != board:
                    errors.append(
                        f"duplicate reported board type {reported_type!r} in "
                        f"{previous} and {board}"
                    )
                else:
                    type_owners[reported_type] = board

            # Check manufacturer consistency with directory structure
            if "/" in board:
                # Board is in a subdirectory (e.g., waveshare/esp32-p4-nano)
                expected_manufacturer = board.split("/")[0]
                if not manufacturer:
                    errors.append(
                        f"{cfg_path}: Board is in '{expected_manufacturer}/' subdirectory, "
                        f"but config.json is missing \"manufacturer\": \"{expected_manufacturer}\""
                    )
                elif manufacturer != expected_manufacturer:
                    errors.append(
                        f"{cfg_path}: manufacturer mismatch, "
                        f"directory is '{expected_manufacturer}/' but config.json has \"{manufacturer}\""
                    )
            else:
                # Board is directly under boards/ directory
                if manufacturer:
                    errors.append(
                        f"{cfg_path}: Board is not in a manufacturer subdirectory, "
                        f"but config.json defines manufacturer \"{manufacturer}\", "
                        f"please move board to main/boards/{manufacturer}/{board}/"
                    )

            for build in cfg.get("builds", []):
                name = _validate_reported_identifier(build.get("name"), 'build "name"')
                full_name = f"{manufacturer}-{name}" if manufacturer else name

                # Unique reported name.
                previous_name = name_owners.get(name)
                if previous_name is not None:
                    errors.append(
                        f"duplicate reported board name {name!r} in "
                        f"{previous_name} and {cfg_path}"
                    )
                else:
                    name_owners[name] = str(cfg_path)

                variants.append({
                    "board": board,
                    "name": name,
                    "full_name": full_name
                })

        except Exception as e:
            errors.append(f"{cfg_path}: {e}")

    # Unique artifact name (manufacturer + name), which maps to the zip file.
    seen_names: dict[str, str] = {}
    for variant in variants:
        previous_board = seen_names.get(variant["full_name"])
        if previous_board is not None:
            errors.append(
                f"duplicate artifact name {variant['full_name']!r} in "
                f"{previous_board} and {variant['board']}"
            )
        else:
            seen_names[variant["full_name"]] = variant["board"]

    # Report all errors at once
    if errors:
        print("\n[ERROR] Found board configuration issues:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        print(file=sys.stderr)
        sys.exit(1)

    return sorted(variants, key=lambda variant: (variant["board"], variant["name"]))



def _find_board_config_candidates(board_type: str) -> list[str]:
    """Find all CONFIG_BOARD_TYPE_xxx candidates for the given board_type."""
    board_path = board_type.strip("/")
    lines = Path("main/CMakeLists.txt").read_text(encoding="utf-8").splitlines()
    candidates: list[str] = []
    branch_symbol: Optional[str] = None
    branch_lines: list[str] = []

    def finish_branch() -> None:
        if branch_symbol is None:
            return

        branch = "\n".join(branch_lines)
        directory_match = re.search(r'set\(BOARD_DIR\s+"([^"]+)"\)', branch)
        directory = directory_match.group(1) if directory_match else None
        if directory == board_path:
            candidates.append(branch_symbol)

    condition_pattern = re.compile(r"^\s*(?:if|elseif)\(([^)]+)\)")
    for line in lines:
        condition_match = condition_pattern.match(line)
        if condition_match:
            finish_branch()
            condition = condition_match.group(1)
            branch_symbol = (
                condition if condition.startswith("CONFIG_BOARD_TYPE_") else None
            )
            branch_lines = []
        elif re.match(r"^\s*(?:else|endif)\b", line):
            finish_branch()
            branch_symbol = None
            branch_lines = []
        elif branch_symbol is not None:
            branch_lines.append(line)
    finish_branch()

    return candidates


def _extract_board_config_from_sdkconfig_append(sdkconfig_append: list[str]) -> Optional[str]:
    """Extract explicit CONFIG_BOARD_TYPE_xxx=y from sdkconfig_append, if present."""
    pattern = re.compile(r"^(CONFIG_BOARD_TYPE_[A-Z0-9_]+)=y$")
    matches = []
    for item in sdkconfig_append:
        m = pattern.match(item.strip())
        if m:
            matches.append(m.group(1))
    if not matches:
        return None
    uniq = list(dict.fromkeys(matches))
    if len(uniq) > 1:
        raise ValueError(f"Multiple board type configs found in sdkconfig_append: {uniq}")
    return uniq[0]


def _symbol_supports_target(symbol: str, target: str) -> bool:
    """Check whether Kconfig symbol depends on given target (e.g. esp32c5)."""
    kconfig_file = Path("main/Kconfig.projbuild")
    if not kconfig_file.exists():
        return False

    target_flag = f"IDF_TARGET_{target.upper()}"
    lines = kconfig_file.read_text(encoding="utf-8").splitlines()

    in_symbol = False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("config "):
            curr_symbol = stripped.split("config ", 1)[1].strip()
            in_symbol = curr_symbol == symbol
            continue
        if in_symbol and stripped.startswith(("config ", "choice ", "endchoice", "menu ", "endmenu")):
            break
        if in_symbol and "depends on" in stripped and target_flag in stripped:
            return True
    return False


def _resolve_board_config(board_type: str, target: str, sdkconfig_append: list[str]) -> str:
    """Resolve CONFIG_BOARD_TYPE_xxx for current board build."""
    explicit = _extract_board_config_from_sdkconfig_append(sdkconfig_append)
    if explicit:
        return explicit

    candidates = _find_board_config_candidates(board_type)
    if not candidates:
        raise ValueError(f"Cannot find board config symbol for {board_type}")
    if len(candidates) == 1:
        return candidates[0]

    by_target = [c for c in candidates if _symbol_supports_target(c, target)]
    if len(by_target) == 1:
        return by_target[0]
    if len(by_target) > 1:
        selected = by_target[0]
        print(
            f"[WARN] Ambiguous board config for {board_type} (target={target}), "
            f"target-matched candidates={by_target}, selecting first: {selected}",
            file=sys.stderr,
        )
        return selected

    target_u = target.upper()
    target_short = target_u.replace("ESP32", "")
    by_name = [
        c for c in candidates
        if target_u in c or f"_{target_short}" in c
    ]
    if len(by_name) == 1:
        return by_name[0]
    if len(by_name) > 1:
        selected = by_name[0]
        print(
            f"[WARN] Ambiguous board config for {board_type} (target={target}), "
            f"name-matched candidates={by_name}, selecting first: {selected}",
            file=sys.stderr,
        )
        return selected

    selected = candidates[0]
    print(
        f"[WARN] Ambiguous board config for {board_type} (target={target}), "
        f"candidates={candidates}, selecting first: {selected}",
        file=sys.stderr,
    )
    return selected


# Kconfig "select" entries are not automatically applied when we simply append
# sdkconfig lines from config.json, so add the required dependencies here to
# mimic menuconfig behaviour.
_AUTO_SELECT_RULES: dict[str, list[str]] = {
    "CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING": [
        "CONFIG_BT_ENABLED=y",
        "CONFIG_BT_BLUEDROID_ENABLED=y",
        "CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y",
        "CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n",
        "CONFIG_BT_BLE_BLUFI_ENABLE=y",
        "CONFIG_MBEDTLS_DHM_C=y",
    ],
}


def _apply_auto_selects(sdkconfig_append: list[str]) -> list[str]:
    """Apply hardcoded auto-select rules to sdkconfig_append."""
    items: list[str] = []
    existing_keys: set[str] = set()

    def _append_if_missing(entry: str) -> None:
        key = entry.split("=", 1)[0]
        if key not in existing_keys:
            items.append(entry)
            existing_keys.add(key)

    # Preserve original order while tracking keys
    for entry in sdkconfig_append:
        _append_if_missing(entry)

    # Apply auto-select rules
    for key, deps in _AUTO_SELECT_RULES.items():
        for entry in sdkconfig_append:
            name, _, value = entry.partition("=")
            if name == key and value.lower().startswith("y"):
                for dep in deps:
                    _append_if_missing(dep)
                break

    return items

################################################################################
# Check board_type in CMakeLists
################################################################################

def _board_type_exists(board_type: str) -> bool:
    return bool(_find_board_config_candidates(board_type))


# Fork 默认固件语言。追加在 defaults 片段末尾（片段本身排在
# sdkconfig.defaults 之后，IDF 中后加载的赋值覆盖先加载的），因此无需改动
# 上游的 sdkconfig.defaults / build.py / Kconfig 即可让全量构建出中文固件。
DEFAULT_LANGUAGE = "zh-CN"
DEFAULT_LANGUAGE_SDKCONFIG = (
    f"CONFIG_LANGUAGE_{DEFAULT_LANGUAGE.replace('-', '_').upper()}=y"
)

################################################################################
# Wake word auto-selection (ported from scripts/build.py)
#
# Upstream removed the Kconfig default that enabled AFE wake words on
# S3/P4+PSRAM targets (dec42c4b) and moved model selection into build.py,
# which picks a model from the build language (zh-* -> nihaoxiaozhi, others
# -> hiesp). build_all.py bypasses build.py, so replicate that logic here to
# keep batch builds producing firmware with a wake word enabled.
################################################################################

_LITE_WAKE_WORD_TARGETS = {"esp32c3", "esp32c5", "esp32c6"}
_AFE_WAKE_WORD_TARGETS = {"esp32s3", "esp32p4", "esp32s31"}


def _sdkconfig_assignments(options: list[str]) -> dict[str, str]:
    """Parse KEY=VALUE sdkconfig lines into a dict."""
    assignments: dict[str, str] = {}
    for option in options:
        key, sep, value = option.partition("=")
        if sep:
            assignments[key] = value
    return assignments


def _board_supports_wake_word(
    target: str,
    sdkconfig_append: list[str],
) -> bool:
    """Respect board opt-outs as well as target and PSRAM requirements."""
    if _sdkconfig_assignments(sdkconfig_append).get("CONFIG_WAKE_WORD_DISABLED") == "y":
        return False
    if target in _LITE_WAKE_WORD_TARGETS:
        return True
    if target not in _AFE_WAKE_WORD_TARGETS | {"esp32"}:
        return False

    defaults: list[str] = []
    for path in (Path("sdkconfig.defaults"), Path(f"sdkconfig.defaults.{target}")):
        if not path.exists():
            continue
        defaults.extend(
            line.strip()
            for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip().startswith("CONFIG_") and "=" in line
        )
    assignments = _sdkconfig_assignments(defaults + list(sdkconfig_append))
    return assignments.get("CONFIG_SPIRAM") == "y"


def _wake_word_sdkconfig_options(wake_word: str, target: str) -> list[str]:
    """Map a wake-word name to implementation and model Kconfig options.

    Mirrors scripts/build.py._wake_word_sdkconfig_options for the two
    auto-selected names: lite targets take the wn9s model with the plain
    ESP wake-word implementation; PSRAM targets take the wn9 *_tts model,
    with AFE on S3/P4/S31.
    """
    normalized = (
        f"wn9s_{wake_word}"
        if target in _LITE_WAKE_WORD_TARGETS
        else f"wn9_{wake_word}_tts"
    )
    implementation = (
        "CONFIG_USE_AFE_WAKE_WORD"
        if target in _AFE_WAKE_WORD_TARGETS
        else "CONFIG_USE_ESP_WAKE_WORD"
    )
    return [
        "CONFIG_WAKE_WORD_DISABLED=n",
        "CONFIG_USE_ESP_WAKE_WORD=n",
        "CONFIG_USE_AFE_WAKE_WORD=n",
        "CONFIG_USE_CUSTOM_WAKE_WORD=n",
        f"{implementation}=y",
        f"CONFIG_SR_WN_{normalized.upper()}=y",
    ]


def _default_wake_word(language: str) -> str:
    """Select the wake word from the build language, as build.py does."""
    return "nihaoxiaozhi" if language in {"zh-CN", "zh-TW"} else "hiesp"


def _regenerate_sdkconfig(target: str, sdkconfig_append: list[str],
                          *, target_changed: bool = False) -> bool:
    """Regenerate sdkconfig from project defaults plus the selected variant.

    Mirrors scripts/build.py._regenerate_sdkconfig: the variant's sdkconfig
    lines are written to a dedicated defaults fragment and ESP-IDF is asked to
    reconfigure using `sdkconfig.defaults` + that fragment. This guarantees a
    clean, reproducible configuration per board (no stale append leftovers),
    while keeping set-target reuse intact.

    Returns True on success, False on failure (caller should skip this board
    but keep building the remaining ones).
    """
    sdkconfig = Path("sdkconfig")
    sdkconfig_old = Path("sdkconfig.old")
    moved_sdkconfig = False
    if sdkconfig.exists():
        if target_changed:
            # set-target already saved the previous config as sdkconfig.old.
            # Discard only the fresh default config it created.
            sdkconfig.unlink()
        else:
            if sdkconfig_old.exists():
                sdkconfig_old.unlink()
            sdkconfig.replace(sdkconfig_old)
            moved_sdkconfig = True

    fragment = Path("build/xiaozhi-build.sdkconfig.defaults")
    fragment.parent.mkdir(parents=True, exist_ok=True)
    fragment.write_text(
        "# Generated by scripts/build_all.py\n"
        + "\n".join(sdkconfig_append)
        + "\n"
        + DEFAULT_LANGUAGE_SDKCONFIG
        + "\n",
        encoding="utf-8",
    )

    defaults = []
    if Path("sdkconfig.defaults").exists():
        defaults.append("sdkconfig.defaults")
    defaults.append(fragment.as_posix())
    if os.system(
        f"idf.py -DIDF_TARGET={target} "
        f"-DSDKCONFIG_DEFAULTS='{';'.join(defaults)}' reconfigure"
    ) != 0:
        print("sdkconfig reconfigure failed", file=sys.stderr)
        # Restore the previous sdkconfig so the build directory is not left in
        # an inconsistent state for subsequent boards.
        if moved_sdkconfig and sdkconfig_old.exists() and not sdkconfig.exists():
            sdkconfig_old.replace(sdkconfig)
        return False
    return True

################################################################################
# Compile implementation
################################################################################

def _read_target(board_type: str, config_filename: str = "config.json") -> Optional[str]:
    """Read the chip target (e.g. esp32/esp32c3) from a board's config.json."""
    cfg_path = _BOARDS_DIR / Path(board_type) / config_filename
    if not cfg_path.exists():
        return None
    try:
        with cfg_path.open(encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg.get("target")
    except Exception:
        return None


def _current_build_target() -> Optional[str]:
    """Read the target already configured in build/CMakeCache.txt, if any.

    Returns the IDF_TARGET string (e.g. 'esp32c3') or None when no valid
    build directory exists yet.
    """
    cache = Path("build/CMakeCache.txt")
    if not cache.exists():
        return None
    try:
        for line in cache.read_text(encoding="utf-8").splitlines():
            if line.startswith("IDF_TARGET:STRING="):
                return line.split("=", 1)[1].strip()
    except Exception:
        return None
    return None


def release(board_type: str, config_filename: str = "config.json", *,
            filter_name: Optional[str] = None,
            out_dir: Optional[str] = None,
            skip_set_target: bool = False,
            idf_version: Optional[tuple[int, int, int]] = None) -> None:
    """Compile and package all/specified variants of the specified board_type

    Args:
        board_type: directory name under main/boards
        config_filename: config.json name (default: config.json)
        filter_name: if specified, only compile the build["name"] that matches
        idf_version: resolved ESP-IDF version; builds with an incompatible
            `idf_version` rule are skipped (with a log line).
    """
    cfg_path = _BOARDS_DIR / Path(board_type) / config_filename
    if not cfg_path.exists():
        print(f"[WARN] {cfg_path} does not exist, skipping {board_type}")
        return

    project_version = get_project_version()
    print(f"Project Version: {project_version} ({cfg_path})")

    with cfg_path.open(encoding='utf-8') as f:
        cfg = json.load(f)
    target = cfg.get("target")
    if not target:
        print(
            f"[WARN] {cfg_path}: missing \"target\", skipping {board_type}",
            file=sys.stderr,
        )
        return
    manufacturer = _get_manufacturer(cfg)

    # Filter builds by optional ESP-IDF version constraint. Variants excluded
    # by the `idf_version` rule still get a summary line so the progress counter
    # stays in sync with the total.
    all_builds = list(cfg.get("builds", []))
    builds = _get_builds_for_idf(cfg, idf_version) if idf_version else all_builds
    if idf_version and len(builds) != len(all_builds):
        for build in all_builds:
            if build in builds:
                continue
            vname = f"{manufacturer}-{build.get('name')}" if manufacturer else build.get("name")
            _write_summary(
                "SKIP", vname,
                f"incompatible with ESP-IDF {'.'.join(str(p) for p in idf_version)}",
            )
    if not builds and not filter_name:
        print(
            f"[WARN] {board_type}: no compatible variants to build, skipping",
            file=sys.stderr,
        )
        return
    if filter_name:
        builds = [b for b in builds if b["name"] == filter_name]
        if not builds:
            print(f"[ERROR] Variant {filter_name} not found in {board_type}'s {config_filename}", file=sys.stderr)
            sys.exit(1)

    for build in builds:
        name = build["name"]

        final_name = f"{manufacturer}-{name}" if manufacturer else name
        # Use the same output directory as zip_bin so the "already exists"
        # skip check is consistent with where the zip is actually written.
        output_base = Path(out_dir) if out_dir else Path("releases")
        output_path = output_base / f"v{project_version}_{final_name}.zip"
        if output_path.exists():
            print(f"Skipping {final_name} because {output_path} already exists")
            _write_summary("SKIP", final_name, "already exists")
            continue

        # Process sdkconfig_append
        build_sdkconfig_append = build.get("sdkconfig_append", [])
        explicit_board_cfg = _extract_board_config_from_sdkconfig_append(build_sdkconfig_append)
        if explicit_board_cfg:
            print(
                f"[INFO] Board config explicitly set in config.json: {explicit_board_cfg}, "
                "skip auto-select.",
            )
            sdkconfig_append = list(build_sdkconfig_append)
        else:
            board_type_config = _resolve_board_config(board_type, target, build_sdkconfig_append)
            sdkconfig_append = [f"{board_type_config}=y"]
            sdkconfig_append.extend(build_sdkconfig_append)
        sdkconfig_append = _apply_auto_selects(sdkconfig_append)

        # Wake word: mirror build.py — auto-select from the build language
        # unless the board opted out via CONFIG_WAKE_WORD_DISABLED=y.
        if _board_supports_wake_word(target, sdkconfig_append):
            wake_word = _default_wake_word(DEFAULT_LANGUAGE)
            sdkconfig_append.extend(_wake_word_sdkconfig_options(wake_word, target))
            print(f"wake word: {wake_word}")

        print("-" * 80)
        print(f"name: {final_name}")
        print(f"target: {target}")
        if manufacturer:
            print(f"manufacturer: {manufacturer}")
        for item in sdkconfig_append:
            print(f"sdkconfig_append: {item}")

        os.environ.pop("IDF_TARGET", None)

        # Call set-target only when necessary:
        # - skip when the previous board in this target group already set it
        # - skip when build/ already targets the same chip (avoids fullclean+cmake)
        target_changed = False
        if skip_set_target:
            print(f"skip set-target for {final_name} (target={target}); reusing previous set-target")
        elif _current_build_target() == target:
            print(f"skip set-target for {final_name} (target={target}); build/ already matches")
        else:
            if os.system(f"idf.py set-target {target}") != 0:
                print(
                    f"set-target failed for {final_name} (target={target}); "
                    f"skipping this build",
                    file=sys.stderr,
                )
                _write_summary("FAIL", final_name, "set-target failed")
                continue
            target_changed = True

        # Regenerate sdkconfig from sdkconfig.defaults + a per-variant fragment,
        # so every board gets a clean, reproducible configuration (no stale
        # append leftovers), independent of set-target reuse.
        if not _regenerate_sdkconfig(target, sdkconfig_append,
                                     target_changed=target_changed):
            print(f"skip {final_name}: sdkconfig reconfigure failed", file=sys.stderr)
            _write_summary("FAIL", final_name, "sdkconfig reconfigure failed")
            continue
        # Build with macro BOARD_NAME defined to name
        if os.system(f"idf.py -DBOARD_NAME={name} -DBOARD_TYPE={board_type} build") != 0:
            print("build failed")
            _write_summary("FAIL", final_name, "build failed")
            continue

        # merge-bin
        if not merge_bin():
            print("merge-bin failed; skipping this build", file=sys.stderr)
            _write_summary("FAIL", final_name, "merge-bin failed")
            continue

        # Zip
        zip_bin(final_name, project_version, out_dir)
        _write_summary("OK", final_name)

################################################################################
# CLI entry
################################################################################

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("board", nargs="?", default=None, help="Board type or 'all'")
    parser.add_argument("-c", "--config", default="config.json", help="Config filename (default: config.json)")
    parser.add_argument("--list-boards", action="store_true", help="List all supported boards and variants")
    parser.add_argument("--json", action="store_true", help="Output in JSON format (use with --list-boards)")
    parser.add_argument("--name", help="Variant name to compile (original name without manufacturer prefix)")
    parser.add_argument("--target", help="Only build boards whose config.json target matches (e.g. esp32, esp32c3)")
    parser.add_argument("--out-dir", default=None, help="Output directory for generated zips (default: releases)")

    args = parser.parse_args()

    # List mode
    if args.list_boards:
        variants = _collect_variants(config_filename=args.config)
        if args.json:
            print(json.dumps(variants))
        else:
            for v in variants:
                print(f"{v['board']}: {v['name']}")
        sys.exit(0)

    # Current directory firmware packaging mode
    if args.board is None:
        if not merge_bin():
            print("merge-bin failed; nothing to package", file=sys.stderr)
            sys.exit(1)
        curr_board_type = get_board_type_from_compile_commands()
        if curr_board_type is None:
            print("Failed to parse board_type from compile_commands.json", file=sys.stderr)
            sys.exit(1)
        project_ver = get_project_version()
        zip_bin(curr_board_type, project_ver)
        sys.exit(0)

    # Compile mode
    board_type_input: str = args.board
    name_filter: Optional[str] = args.name

    # Detect ESP-IDF version once; variants with an incompatible `idf_version`
    # rule will be skipped by release() with a log line.
    idf_version = _detect_idf_version()
    print(
        f"[INFO] ESP-IDF version: {'.'.join(str(p) for p in idf_version)}"
    )

    # Check board_type in CMakeLists
    if board_type_input != "all" and not _board_type_exists(board_type_input):
        print(f"[ERROR] board_type {board_type_input} not found in main/CMakeLists.txt", file=sys.stderr)
        sys.exit(1)

    variants_all = _collect_variants(config_filename=args.config)

    # Filter board_type list
    if board_type_input == "all":
        target_board_types = {v["board"] for v in variants_all}
    else:
        target_board_types = {board_type_input}

    # Optional filter by chip target (e.g. only esp32)
    if args.target:
        filtered = set()
        for bt in target_board_types:
            if _read_target(bt, args.config) == args.target:
                filtered.add(bt)
        skipped = target_board_types - filtered
        if skipped:
            print(f"[INFO] --target {args.target}: skipping {len(skipped)} board(s) with different target")
        target_board_types = filtered

    if not target_board_types:
        print(f"[WARN] No boards matched target '{args.target}'; nothing to build", file=sys.stderr)
        sys.exit(0)

    # Pre-compute the total number of variants (ALL variants, including ones
    # that will be skipped due to an incompatible `idf_version` rule). Every
    # variant outcome is written to the summary log, so the denominator here
    # matches the number of summary lines exactly.
    total_variants = 0
    for bt in sorted(target_board_types):
        cfg_path = _BOARDS_DIR / bt / args.config
        if not cfg_path.exists():
            continue
        try:
            with cfg_path.open(encoding="utf-8") as f:
                cfg = json.load(f)
            total_variants += len(cfg.get("builds", []))
        except Exception:
            pass
    _set_summary_total(total_variants)

    # Group boards by target so set-target runs once per chip target.
    # Within each target group, only the first board triggers set-target;
    # subsequent boards reuse the already-set target to save fullclean+cmake.
    boards_by_target: dict[Optional[str], list[str]] = {}
    for bt in sorted(target_board_types):
        t = _read_target(bt, args.config)
        boards_by_target.setdefault(t, []).append(bt)

    for t, boards in boards_by_target.items():
        for idx, bt in enumerate(boards):
            if not _board_type_exists(bt):
                print(
                    f"[ERROR] board_type {bt} not found in main/CMakeLists.txt; skipping",
                    file=sys.stderr,
                )
                continue
            cfg_path = _BOARDS_DIR / bt / args.config
            if bt == board_type_input and not cfg_path.exists():
                print(f"Board {bt} has no {args.config} config file, skipping")
                continue
            skip_set_target = (idx > 0)  # reuse set-target from the first board of this target
            release(
                bt,
                config_filename=args.config,
                filter_name=name_filter if bt == board_type_input else None,
                out_dir=args.out_dir,
                skip_set_target=skip_set_target,
                idf_version=idf_version,
            )