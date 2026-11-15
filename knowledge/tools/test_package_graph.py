# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Checks for writing a generated graph into a package.

Run: python -B -m unittest knowledge/tools/test_package_graph.py
"""
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import package_graph as subject  # noqa: E402


class PackageGraph(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = pathlib.Path(self.temporary.name)
        self.sources, self.folder = root / "sources-root", root / "pack"
        (self.sources / "authored").mkdir(parents=True)
        (self.sources / "vendor").mkdir()
        (self.sources / "authored/policy.txt").write_bytes(b"policy\n")
        (self.sources / "vendor/header.h").write_bytes(b"header\n")
        self.folder.mkdir()
        (self.folder / "foundation-manifest.json").write_bytes(b"{}\n")
        (self.folder / "knowledge-pack.json").write_text(json.dumps({
            "revision": 3, "inputs": {"architecture": {"path": "graph.json", "sha256": "0" * 64}},
            "coverage": {"capability_count": 0}}, indent=2) + "\n", encoding="utf-8", newline="\n")
        self.graph = root / "generated.json"
        self.write({"capabilities": [{"id": "one"}, {"id": "two"}]})

    def write(self, extra, policy=b"policy\n"):
        value = {
            "targets": [{"foundation_sha256": subject.sha256(b"{}\n")}],
            "sources": [
                {"id": "policy", "path": "authored/policy.txt", "sha256": subject.sha256(policy), "revision": "1", "authority": "bkpt_policy"},
                {"id": "header", "path": "vendor/header.h", "sha256": subject.sha256(b"header\n"), "revision": "1", "authority": "manufacturer_sdk"}],
            **extra}
        self.graph.write_text(json.dumps(value), encoding="utf-8")
        return subject.sha256(self.graph.read_bytes())

    def review(self, digest, decision="accept"):
        path = pathlib.Path(self.temporary.name) / "review.json"
        path.write_text(json.dumps({"bundle_sha256": digest, "decision": decision}), encoding="utf-8")
        return path

    def test_the_graph_its_inputs_and_the_manifest_are_written_in_place(self):
        digest = subject.sha256(self.graph.read_bytes())
        result = subject.package(self.graph, self.folder, self.sources, self.review(digest), 4)
        self.assertEqual((self.folder / "graph.json").read_bytes(), self.graph.read_bytes())
        inputs = json.loads((self.folder / "source-inputs.json").read_text(encoding="utf-8"))
        self.assertEqual([record["id"] for record in inputs["owned"]], ["policy"])
        self.assertEqual([record["id"] for record in inputs["external"]], ["header"])
        # Only what BKPT authored is copied into the package.
        self.assertEqual((self.folder / "sources/authored/policy.txt").read_bytes(), b"policy\n")
        self.assertFalse((self.folder / "sources/vendor").exists())
        pack = json.loads((self.folder / "knowledge-pack.json").read_text(encoding="utf-8"))
        self.assertEqual((pack["inputs"]["architecture"], pack["coverage"]["capability_count"], pack["revision"]),
                         ({"path": "graph.json", "sha256": digest}, 2, 4))
        self.assertEqual(pack["inputs"]["architecture_review"]["path"], "graph.review.json")
        self.assertTrue(result["review_binds_the_graph"])
        # A second graph replaces the first, and says that the record on disk no longer binds it.
        self.write({"capabilities": [{"id": "one"}]})
        result = subject.package(self.graph, self.folder, self.sources)
        self.assertEqual((result["capabilities"], result["review_binds_the_graph"], result["revision"]), (1, False, 4))

    def test_a_review_of_other_bytes_and_an_older_revision_are_refused(self):
        for review, revision in ((self.review("1" * 64), None), (self.review(subject.sha256(self.graph.read_bytes()), "reject"), None),
                                 (None, 2)):
            with self.assertRaises(SystemExit):
                subject.package(self.graph, self.folder, self.sources, review, revision)
        self.assertFalse((self.folder / "graph.json").exists())

    def test_a_source_that_changed_is_refused(self):
        subject.package(self.graph, self.folder, self.sources)
        # The pinned file differs from the graph's hash.
        (self.sources / "vendor/header.h").write_bytes(b"other\n")
        with self.assertRaises(SystemExit):
            subject.package(self.graph, self.folder, self.sources)
        (self.sources / "vendor/header.h").write_bytes(b"header\n")
        # An authored source the package already holds under the same path may not change.
        (self.sources / "authored/policy.txt").write_bytes(b"changed\n")
        self.write({"capabilities": []}, policy=b"changed\n")
        with self.assertRaises(SystemExit):
            subject.package(self.graph, self.folder, self.sources)

    def test_a_graph_on_another_foundation_is_refused(self):
        (self.folder / "foundation-manifest.json").write_bytes(b"{ }\n")
        with self.assertRaises(SystemExit):
            subject.package(self.graph, self.folder, self.sources)


if __name__ == "__main__":
    unittest.main()
