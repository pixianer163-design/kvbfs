"""AgentFS MCP Server — expose AgentFS features to AI agents via MCP protocol."""

import json
import os
import stat
import time

from mcp.server.fastmcp import FastMCP

MOUNT = os.environ.get("KVBFS_MOUNT", "/tmp/kvbfs_e2e_mount")

mcp = FastMCP("agentfs")


def _check_mount() -> None:
    """Raise if the mount point doesn't exist."""
    if not os.path.isdir(MOUNT):
        raise RuntimeError(
            f"KVBFS mount not found at {MOUNT}. "
            "Set KVBFS_MOUNT env var or mount AgentFS first."
        )


@mcp.tool()
def semantic_search(query: str, top_k: int = 5) -> str:
    """Search files by semantic meaning using AgentFS's built-in embedding index.

    Writes a query to the virtual .agentfs control file and reads back
    JSON results with relevance scores, file paths, and text summaries.
    Requires AgentFS compiled with CFS_MEMORY=ON.
    """
    _check_mount()
    ctl = os.path.join(MOUNT, ".agentfs")
    if not os.path.exists(ctl):
        return json.dumps({
            "status": "error",
            "message": ".agentfs not found — compile with CFS_MEMORY=ON",
        })

    try:
        fd = os.open(ctl, os.O_RDWR)
        try:
            os.write(fd, query.encode("utf-8"))
            os.lseek(fd, 0, os.SEEK_SET)
            data = os.read(fd, 65536).decode("utf-8")
        finally:
            os.close(fd)

        result = json.loads(data)
        # Trim results to top_k
        if "results" in result and len(result["results"]) > top_k:
            result["results"] = result["results"][:top_k]
        return json.dumps(result, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"status": "error", "message": str(e)})


@mcp.tool()
def get_file_versions(path: str) -> str:
    """Get the version history of a file in AgentFS.

    Returns current version number and a list of all version snapshots
    with their sizes and modification times.
    """
    _check_mount()
    full = os.path.join(MOUNT, path.lstrip("/"))
    if not os.path.exists(full):
        return json.dumps({"status": "error", "message": f"File not found: {path}"})

    try:
        version = os.getxattr(full, "agentfs.version").decode()
        versions_raw = os.getxattr(full, "agentfs.versions").decode()
        versions = json.loads(versions_raw)
        return json.dumps({
            "status": "ok",
            "path": path,
            "current_version": int(version),
            "versions": versions,
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"status": "error", "message": str(e)})


@mcp.tool()
def get_file_metadata(path: str) -> str:
    """Get all extended attributes (xattr) metadata for a file.

    Returns both user-defined xattrs and virtual agentfs.* xattrs.
    """
    _check_mount()
    full = os.path.join(MOUNT, path.lstrip("/"))
    if not os.path.exists(full):
        return json.dumps({"status": "error", "message": f"File not found: {path}"})

    try:
        attrs = os.listxattr(full)
        metadata = {}
        for attr in attrs:
            try:
                val = os.getxattr(full, attr)
                metadata[attr] = val.decode("utf-8", errors="replace")
            except OSError:
                metadata[attr] = "<unreadable>"

        # Also read virtual xattrs
        for vattr in ["agentfs.version", "agentfs.versions"]:
            if vattr not in metadata:
                try:
                    val = os.getxattr(full, vattr)
                    metadata[vattr] = val.decode("utf-8", errors="replace")
                except OSError:
                    pass

        return json.dumps({
            "status": "ok",
            "path": path,
            "metadata": metadata,
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"status": "error", "message": str(e)})


@mcp.tool()
def set_file_metadata(path: str, key: str, value: str) -> str:
    """Set an extended attribute (xattr) on a file.

    The key should use the 'user.' namespace prefix (e.g., 'user.author').
    Virtual 'agentfs.*' attributes are read-only and cannot be set.
    """
    _check_mount()
    full = os.path.join(MOUNT, path.lstrip("/"))
    if not os.path.exists(full):
        return json.dumps({"status": "error", "message": f"File not found: {path}"})

    if key.startswith("agentfs."):
        return json.dumps({
            "status": "error",
            "message": "agentfs.* attributes are read-only",
        })

    try:
        os.setxattr(full, key, value.encode("utf-8"))
        return json.dumps({"status": "ok", "path": path, "key": key, "value": value})
    except Exception as e:
        return json.dumps({"status": "error", "message": str(e)})


@mcp.tool()
def get_events(max_lines: int = 100) -> str:
    """Get recent file system change events from the .events virtual file.

    Returns JSON Lines of recent events (create, write, unlink, mkdir, etc.).
    Requires AgentFS compiled with CFS_MEMORY=ON.
    """
    _check_mount()
    events_path = os.path.join(MOUNT, ".events")
    if not os.path.exists(events_path):
        return json.dumps({
            "status": "error",
            "message": ".events not found — compile with CFS_MEMORY=ON",
        })

    try:
        fd = os.open(events_path, os.O_RDONLY)
        try:
            data = os.read(fd, 256 * 1024).decode("utf-8", errors="replace")
        finally:
            os.close(fd)

        lines = [l for l in data.strip().split("\n") if l]
        if len(lines) > max_lines:
            lines = lines[-max_lines:]

        events = []
        for line in lines:
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue

        return json.dumps({
            "status": "ok",
            "count": len(events),
            "events": events,
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"status": "error", "message": str(e)})


@mcp.tool()
def list_files(path: str = "/", include_metadata: bool = False) -> str:
    """List directory contents with file information.

    Optionally includes xattr metadata for each file.
    """
    _check_mount()
    full = os.path.join(MOUNT, path.lstrip("/"))
    if not os.path.isdir(full):
        return json.dumps({"status": "error", "message": f"Not a directory: {path}"})

    try:
        entries = []
        for entry in os.scandir(full):
            info = {
                "name": entry.name,
                "is_dir": entry.is_dir(follow_symlinks=False),
                "is_file": entry.is_file(follow_symlinks=False),
                "is_symlink": entry.is_symlink(),
            }
            try:
                st = entry.stat(follow_symlinks=False)
                info["size"] = st.st_size
                info["mtime"] = int(st.st_mtime)
                info["mode"] = oct(stat.S_IMODE(st.st_mode))
            except OSError:
                pass

            if include_metadata and entry.is_file(follow_symlinks=False):
                try:
                    attrs = os.listxattr(entry.path)
                    meta = {}
                    for attr in attrs:
                        try:
                            val = os.getxattr(entry.path, attr)
                            meta[attr] = val.decode("utf-8", errors="replace")
                        except OSError:
                            pass
                    if meta:
                        info["metadata"] = meta
                except OSError:
                    pass

            entries.append(info)

        entries.sort(key=lambda e: (not e["is_dir"], e["name"]))

        return json.dumps({
            "status": "ok",
            "path": path,
            "count": len(entries),
            "entries": entries,
        }, ensure_ascii=False)
    except Exception as e:
        return json.dumps({"status": "error", "message": str(e)})


if __name__ == "__main__":
    mcp.run()
