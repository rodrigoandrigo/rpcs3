#!/usr/bin/env python3
"""Verify Dozen accepts VK_GOOGLE_decorate_string as SPIR-V metadata only."""
from pathlib import Path
import json
import re
import unittest
import xml.etree.ElementTree as ET

TEST_DIR = Path(__file__).resolve().parent
MESA_ROOT = TEST_DIR.parents[3]
DZN_DEVICE = TEST_DIR.parent / "dzn_device.c"
DZN_PIPELINE = TEST_DIR.parent / "dzn_pipeline.c"
SPIRV_PARSER = MESA_ROOT / "src/compiler/spirv/spirv_to_nir.c"
SPIRV_VARIABLES = MESA_ROOT / "src/compiler/spirv/vtn_variables.c"
SPIRV_GRAMMAR = MESA_ROOT / "src/compiler/spirv/spirv.core.grammar.json"
REGISTRY = MESA_ROOT / "src/vulkan/registry/vk.xml"


class GoogleDecorateStringSupportTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = DZN_DEVICE.read_text(encoding="utf-8")
        cls.pipeline = DZN_PIPELINE.read_text(encoding="utf-8")
        cls.parser = SPIRV_PARSER.read_text(encoding="utf-8")
        cls.variables = SPIRV_VARIABLES.read_text(encoding="utf-8")
        cls.grammar = json.loads(SPIRV_GRAMMAR.read_text(encoding="utf-8"))
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_dozen_advertises_the_extension(self):
        self.assertRegex(
            self.device,
            r"\.GOOGLE_decorate_string\s*=\s*true\b",
        )

    def test_registry_declares_metadata_extension_and_spirv_enablement(self):
        extension = self.registry.find(
            "./extensions/extension[@name='VK_GOOGLE_decorate_string']"
        )
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get("nofeatures"), "true")
        self.assertFalse(
            any(
                requirement.findall("./command") or requirement.findall("./feature")
                for requirement in extension.findall("./require")
            )
        )
        spirv_extension = self.registry.find(
            "./spirvextensions/spirvextension[@name='SPV_GOOGLE_decorate_string']"
        )
        self.assertIsNotNone(spirv_extension)
        self.assertTrue(
            any(
                node.get("extension") == "VK_GOOGLE_decorate_string"
                for node in spirv_extension.findall("./enable")
            )
        )

    def test_spirv_grammar_contains_both_google_aliases(self):
        instructions = {
            instruction["opname"]: instruction
            for instruction in self.grammar["instructions"]
        }
        for opname, alias in (
            ("OpDecorateString", "OpDecorateStringGOOGLE"),
            ("OpMemberDecorateString", "OpMemberDecorateStringGOOGLE"),
        ):
            instruction = instructions[opname]
            self.assertIn(alias, instruction.get("aliases", []))
            self.assertIn(
                "SPV_GOOGLE_decorate_string",
                instruction.get("extensions", []),
            )

    def test_parser_stores_string_decorations_and_ignores_user_metadata(self):
        self.assertIn("case SpvOpDecorateString:", self.parser)
        self.assertIn("case SpvOpMemberDecorateString:", self.parser)
        self.assertIn("dec->operands = w;", self.parser)
        for source in (self.parser, self.variables):
            self.assertIn("SpvDecorationUserSemantic", source)
            self.assertIn("SpvDecorationUserTypeGOOGLE", source)
            self.assertIn("can safely be ignored", source)

    def test_dozen_pipeline_uses_the_shared_spirv_parser(self):
        self.assertIn("dxil_spirv_nir_get_spirv_options()", self.pipeline)
        self.assertIn("vk_pipeline_shader_stage_to_nir(", self.pipeline)


if __name__ == "__main__":
    unittest.main()

