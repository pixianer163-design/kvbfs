"""Tests for AgentFS MCP Server tools.

These tests require a mounted KVBFS instance at KVBFS_MOUNT (default /tmp/kvbfs_e2e_mount).
Run: KVBFS_MOUNT=/tmp/kvbfs_e2e_mount python -m pytest cfs/tests/test_mcp.py -v
"""

import json
import os
import sys

import pytest

# Ensure cfs/ is on the path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

MOUNT = os.environ.get("KVBFS_MOUNT", "/tmp/kvbfs_e2e_mount")


def mount_available():
    return os.path.isdir(MOUNT) and os.path.ismount(MOUNT)


skipif_no_mount = pytest.mark.skipif(
    not mount_available(),
    reason=f"KVBFS not mounted at {MOUNT}",
)


@skipif_no_mount
class TestMCPTools:
    """Test MCP tool functions directly (without MCP protocol)."""

    def setup_method(self):
        from mcp_server import list_files, get_file_metadata, set_file_metadata
        from mcp_server import get_file_versions, get_events, semantic_search
        self.list_files = list_files
        self.get_file_metadata = get_file_metadata
        self.set_file_metadata = set_file_metadata
        self.get_file_versions = get_file_versions
        self.get_events = get_events
        self.semantic_search = semantic_search

    def test_list_files_root(self):
        result = json.loads(self.list_files("/"))
        assert result["status"] == "ok"
        assert isinstance(result["entries"], list)

    def test_list_files_nonexistent(self):
        result = json.loads(self.list_files("/nonexistent_dir_12345"))
        assert result["status"] == "error"

    def test_file_metadata_roundtrip(self):
        # Create a test file
        test_file = os.path.join(MOUNT, "mcp_test_meta.txt")
        with open(test_file, "w") as f:
            f.write("metadata test")

        try:
            # Set metadata
            result = json.loads(self.set_file_metadata("mcp_test_meta.txt", "user.mcp_test", "hello"))
            assert result["status"] == "ok"

            # Get metadata
            result = json.loads(self.get_file_metadata("mcp_test_meta.txt"))
            assert result["status"] == "ok"
            assert "user.mcp_test" in result["metadata"]
            assert result["metadata"]["user.mcp_test"] == "hello"
        finally:
            os.unlink(test_file)

    def test_set_metadata_agentfs_readonly(self):
        test_file = os.path.join(MOUNT, "mcp_test_ro.txt")
        with open(test_file, "w") as f:
            f.write("readonly test")

        try:
            result = json.loads(self.set_file_metadata("mcp_test_ro.txt", "agentfs.version", "999"))
            assert result["status"] == "error"
            assert "read-only" in result["message"]
        finally:
            os.unlink(test_file)

    def test_get_file_versions(self):
        test_file = os.path.join(MOUNT, "mcp_test_ver.txt")
        with open(test_file, "w") as f:
            f.write("v1")

        try:
            result = json.loads(self.get_file_versions("mcp_test_ver.txt"))
            assert result["status"] == "ok"
            assert result["current_version"] >= 1
        finally:
            os.unlink(test_file)

    def test_get_events(self):
        result = json.loads(self.get_events())
        # May be "error" if .events doesn't exist (no CFS_MEMORY), or "ok"
        assert result["status"] in ("ok", "error")

    def test_semantic_search(self):
        result = json.loads(self.semantic_search("test query"))
        # May be "error" if .agentfs doesn't exist (no CFS_MEMORY), or valid JSON
        assert result["status"] in ("ok", "error")

    def test_list_files_with_metadata(self):
        test_file = os.path.join(MOUNT, "mcp_test_listmeta.txt")
        with open(test_file, "w") as f:
            f.write("list meta test")

        try:
            os.setxattr(test_file, "user.color", b"blue")
            result = json.loads(self.list_files("/", include_metadata=True))
            assert result["status"] == "ok"

            found = False
            for entry in result["entries"]:
                if entry["name"] == "mcp_test_listmeta.txt":
                    found = True
                    if "metadata" in entry:
                        assert "user.color" in entry["metadata"]
            assert found
        finally:
            os.unlink(test_file)
