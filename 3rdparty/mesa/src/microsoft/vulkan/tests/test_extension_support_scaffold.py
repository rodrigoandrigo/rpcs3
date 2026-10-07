#!/usr/bin/env python3
# Copyright © Microsoft Corporation
# SPDX-License-Identifier: MIT
"""Guard the extension implementation scaffold and current advertisement policy."""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
sys.path.insert(0, str(TEST_DIR))

import extension_support_scaffold as scaffold  # noqa: E402


class ExtensionSupportScaffoldTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backlog = TEST_DIR / "extension_support_backlog.json"
        cls.output = TEST_DIR / "extension_support_scaffold.json"
        cls.data = scaffold.build_scaffold(
            cls.backlog,
            MESA_ROOT / "src/vulkan/registry/vk.xml",
            MESA_ROOT / "src/microsoft/vulkan/dzn_device.c",
        )

    def test_all_reviewed_pending_candidates_have_a_scaffold(self):
        self.assertEqual(self.data["counts"]["reviewed_candidates"], 175)
        self.assertEqual(self.data["counts"]["implemented_in_review"], 42)
        self.assertEqual(self.data["counts"]["pending_scaffolded"], 133)
        self.assertEqual(len(self.data["extensions"]), 133)
        self.assertEqual(
            len({entry["field"] for entry in self.data["extensions"]}), 133
        )

    def test_no_pending_candidate_is_advertised(self):
        self.assertEqual(self.data["counts"]["pending_advertised_in_source"], 0)
        for entry in self.data["extensions"]:
            self.assertFalse(entry["currently_advertised_in_dozen_source"], entry["field"])
            self.assertEqual(entry["review_status"], "rejected_not_advertised")

    def test_registry_payloads_are_complete_for_every_pending_candidate(self):
        self.assertEqual(self.data["counts"]["without_registry_entry"], 0)
        for entry in self.data["extensions"]:
            registry = entry["registry"]
            self.assertTrue(registry["registry_entry"], entry["name"])
            self.assertIn("depends", registry)
            self.assertIn("promotedto", registry)
            self.assertIn("required_commands", registry)
            self.assertIn("required_types", registry)
            self.assertIn("feature_structures", registry)
            self.assertIn("feature_members", registry)
            self.assertIn("pnext_structures", registry)

    def test_every_candidate_has_explicit_gates_and_verification_work(self):
        for entry in self.data["extensions"]:
            ids = {item["id"] for item in entry["work_items"]}
            self.assertIn("backend_capability_probe", ids, entry["field"])
            self.assertIn("dependency_and_advertisement_gate", ids, entry["field"])
            self.assertIn("static_and_runtime_tests", ids, entry["field"])
            self.assertTrue(all(item["status"] == "todo" for item in entry["work_items"]))
            if entry["registry"]["required_commands"]:
                self.assertIn("commands_and_dispatch", ids, entry["field"])
            if entry["registry"]["feature_members"]:
                self.assertIn("feature_queries_and_device_enablement", ids, entry["field"])
            if entry["registry"]["property_payload_types"]:
                self.assertIn("property_queries", ids, entry["field"])
            if entry["registry"]["pnext_structures"]:
                self.assertIn("pnext_and_object_semantics", ids, entry["field"])

    def test_checked_in_generated_manifest_is_current(self):
        checked_in = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual(checked_in, self.data)

    def test_manifest_never_claims_support(self):
        self.assertTrue(self.data["policy"]["pending_extensions_must_not_be_advertised"])
        self.assertTrue(self.data["policy"]["common_runtime_or_generated_entrypoints_are_not_by_themselves_backend_support"])
        for entry in self.data["extensions"]:
            self.assertNotIn("supported", entry)
            self.assertFalse(any(item.get("status") == "done" for item in entry["work_items"]))


if __name__ == "__main__":
    unittest.main()

