#!/usr/bin/env python3
"""Apply Playback-owned T5AI config and partition overlays during builds."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import ModuleType
from typing import Sequence


def _load_platform_build(script: Path) -> ModuleType:
    platform_root = str(script.parent)
    if platform_root not in sys.path:
        sys.path.insert(0, platform_root)

    spec = importlib.util.spec_from_file_location("playback_t5ai_build_example", script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load T5AI build wrapper: {script}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _replace_symlink(link: Path, target: Path) -> None:
    if link.is_symlink() or link.is_file():
        link.unlink()
    elif link.exists():
        shutil.rmtree(link)
    link.symlink_to(target, target_is_directory=True)


def _prepare_overlay(
    script: Path,
    build_param_path: Path,
    partition_file: Path,
) -> tuple[Path, Path]:
    source_platform = script.parent
    source_project = source_platform / "t5_os" / "projects" / "tuya_app"
    if not source_project.is_dir():
        raise RuntimeError(f"T5AI tuya_app project is missing: {source_project}")

    overlay_platform = (
        build_param_path / "playback_t5ai_overlay" / "platform" / "T5AI"
    )
    overlay_t5_os = overlay_platform / "t5_os"
    overlay_project = overlay_t5_os / "projects" / "tuya_app"
    overlay_project.parent.mkdir(parents=True, exist_ok=True)

    if overlay_project.exists():
        shutil.rmtree(overlay_project)
    shutil.copytree(source_project, overlay_project)

    overlay_partition = (
        overlay_project / "partitions" / "bk7258" / "auto_partitions.csv"
    )
    overlay_partition.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(partition_file, overlay_partition)

    overlay_t5_os.mkdir(parents=True, exist_ok=True)
    _replace_symlink(overlay_t5_os / "build", source_platform / "t5_os" / "build")
    return overlay_platform, overlay_project


def _combined_fragment(
    module: ModuleType,
    project_fragment: Path,
    build_param_path: str,
    param_data: dict[str, object],
) -> Path:
    _, uart_port = module._board_log_uart_intent(param_data)
    if uart_port is None:
        return project_fragment

    combined = Path(build_param_path) / "playback_t5ai_sdkconfig.append"
    contents = project_fragment.read_text(encoding="utf-8")
    if contents and not contents.endswith("\n"):
        contents += "\n"
    contents += f"CONFIG_UART_PRINT_PORT={uart_port}\n"
    combined.write_text(contents, encoding="utf-8")
    return combined


def _configuration_digest(fragment: Path, partition_file: Path) -> str:
    digest = hashlib.sha256()
    for path in (fragment, partition_file):
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _install_project_hooks(
    module: ModuleType,
    project_fragment: Path,
    partition_file: Path,
    overlay_platform: Path,
    overlay_project: Path,
) -> None:
    original_set_environment = module.set_environment
    original_sync = module.sync_board_log_uart

    def set_environment(
        root: str,
        build_param_path: str,
        param_data: dict[str, object],
    ) -> None:
        original_set_environment(root, build_param_path, param_data)
        os.environ["TUYA_PROJECT_DIR"] = str(overlay_platform)

    def set_board_log_uart_env(
        build_param_path: str,
        param_data: dict[str, object],
    ) -> None:
        fragment = _combined_fragment(
            module,
            project_fragment,
            build_param_path,
            param_data,
        )
        os.environ["TUYA_APP_SDKCONFIG_APPEND"] = str(fragment)

    def sync_board_log_uart(
        build_root: str,
        param_data: dict[str, object],
    ) -> None:
        original_sync(build_root, param_data)

        fragment = Path(os.environ["TUYA_APP_SDKCONFIG_APPEND"])
        digest = _configuration_digest(fragment, partition_file)
        build_dir = Path(build_root) / "build"
        generated_config = build_dir / "bk7258"
        stamp = build_dir / ".playback_t5ai_configuration_sha256"
        previous = stamp.read_text(encoding="utf-8").strip() if stamp.is_file() else None

        if previous != digest and generated_config.is_dir():
            print(
                "[playback] T5AI configuration changed "
                f"({previous or 'unset'} -> {digest[:12]}), regenerating"
            )
            shutil.rmtree(generated_config, ignore_errors=True)

        build_dir.mkdir(parents=True, exist_ok=True)
        stamp.write_text(digest, encoding="utf-8")

    def _quote(value: str) -> str:
        # os.system on Windows goes through cmd.exe, which cannot parse
        # POSIX '"'"' nesting; mimic the stock SDK's plain '{cmd}' wrapping
        # with forward-slash paths instead of shlex.quote.
        if os.name == "nt":
            return value.replace("\\", "/")
        return shlex.quote(value)

    def build(
        build_root: str,
        toolchain_folder_path: str,
        bash_path: str,
        target: str,
        app_name: str,
        app_ver: str,
    ) -> bool:
        build_root = build_root.replace("\\", "/")
        toolchain_folder_path = toolchain_folder_path.replace("\\", "/")
        command = (
            f"export TUYA_TOOLCHAIN_PATH={_quote(toolchain_folder_path)}; "
            f"cd {_quote(build_root)}; "
            f"make {_quote(target)} PROJECT=tuya_app "
            f"PROJECT_DIR={_quote(str(overlay_project))} "
            f"APP_NAME={_quote(app_name)} "
            f"APP_VERSION={_quote(app_ver)} -j"
        )
        if os.path.exists(bash_path):
            if os.name == "nt":
                command = f"{bash_path} -c '{command}'"
            else:
                command = f"{shlex.quote(bash_path)} -c {shlex.quote(command)}"
        return module.do_subprocess(command) == 0

    def clean(build_root: str, toolchain_folder_path: str, bash_path: str) -> None:
        build_root = build_root.replace("\\", "/")
        toolchain_folder_path = toolchain_folder_path.replace("\\", "/")
        command = (
            f"export TUYA_TOOLCHAIN_PATH={_quote(toolchain_folder_path)}; "
            f"cd {_quote(build_root)}; "
            f"make clean PROJECT=tuya_app "
            f"PROJECT_DIR={_quote(str(overlay_project))}"
        )
        if os.path.exists(bash_path):
            if os.name == "nt":
                command = f"{bash_path} -c '{command}'"
            else:
                command = f"{shlex.quote(bash_path)} -c {shlex.quote(command)}"
        module.do_subprocess(command)

    module.set_environment = set_environment
    module.set_board_log_uart_env = set_board_log_uart_env
    module.sync_board_log_uart = sync_board_log_uart
    module.build = build
    module.clean = clean


def _run_t5ai_build(
    project_fragment: Path,
    partition_file: Path,
    command: Sequence[str],
    script_index: int,
) -> int:
    script = Path(command[script_index]).resolve()
    if script_index + 1 >= len(command):
        raise RuntimeError("T5AI build wrapper is missing its build-parameter path")
    build_param_path = Path(command[script_index + 1]).resolve()
    overlay_platform, overlay_project = _prepare_overlay(
        script,
        build_param_path,
        partition_file,
    )

    module = _load_platform_build(script)
    _install_project_hooks(
        module,
        project_fragment,
        partition_file,
        overlay_platform,
        overlay_project,
    )

    lock_name = hashlib.sha256(str(script.parent).encode("utf-8")).hexdigest()[:16]
    lock_path = Path(tempfile.gettempdir()) / f"playback-t5ai-{lock_name}.lock"
    original_argv = sys.argv
    try:
        with lock_path.open("w", encoding="utf-8") as lock_file:
            try:
                import fcntl

                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            except ImportError:
                pass

            sys.argv = [str(script), *command[script_index + 1 :]]
            module.main()
    except SystemExit as exit_status:
        return int(exit_status.code or 0)
    finally:
        sys.argv = original_argv
    return 0


def main() -> int:
    if len(sys.argv) < 4:
        print(
            "usage: t5ai_custom_launcher.py SDKCONFIG PARTITIONS COMMAND...",
            file=sys.stderr,
        )
        return 2

    project_fragment = Path(sys.argv[1]).resolve()
    partition_file = Path(sys.argv[2]).resolve()
    command = sys.argv[3:]
    for label, path in (
        ("sdkconfig fragment", project_fragment),
        ("partition table", partition_file),
    ):
        if not path.is_file():
            print(f"missing T5AI {label}: {path}", file=sys.stderr)
            return 2

    for index, argument in enumerate(command):
        if Path(argument).name == "build_example.py":
            return _run_t5ai_build(
                project_fragment,
                partition_file,
                command,
                index,
            )

    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
