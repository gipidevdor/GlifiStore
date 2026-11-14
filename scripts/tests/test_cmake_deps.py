from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from engineering.tools.validate_cmake_deps import (
    header_closure_errors,
    installed_headers,
    inventory_errors,
)


ROOT = Path(__file__).resolve().parents[2]


def install_rule(*sources: str, destination: str = "glifistore") -> str:
    files = "\n".join(f"    {source}" for source in sources)
    return (
        f'install(FILES\n{files}\n    DESTINATION "${{CMAKE_INSTALL_INCLUDEDIR}}/{destination}"\n'
        "    COMPONENT Development\n)\n"
    )


class InstalledHeaderClosureTests(unittest.TestCase):
    def make_tree(self, headers: dict[str, str]) -> Path:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-header-closure-test-")
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        for relative, text in headers.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return root

    def test_repository_installs_every_header_its_installed_headers_include(self) -> None:
        cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        headers, layout_errors = installed_headers(cmake_text)

        self.assertEqual(layout_errors, [])
        self.assertIn("include/glifistore/store/store.hpp", headers)
        self.assertEqual(header_closure_errors(ROOT, headers), [])
        self.assertEqual(inventory_errors(ROOT, headers), [])

    def test_header_included_by_an_installed_header_must_be_installed(self) -> None:
        root = self.make_tree(
            {
                "include/glifistore/a.hpp": '#include "glifistore/b.hpp"\n#include <vector>\n',
                "include/glifistore/b.hpp": "#pragma once\n",
            }
        )

        errors = header_closure_errors(root, ["include/glifistore/a.hpp"])

        self.assertEqual(len(errors), 1)
        self.assertIn("include/glifistore/a.hpp includes glifistore/b.hpp", errors[0])
        self.assertIn("does not install include/glifistore/b.hpp", errors[0])
        self.assertEqual(
            header_closure_errors(root, ["include/glifistore/a.hpp", "include/glifistore/b.hpp"]),
            [],
        )

    def test_closure_follows_includes_through_uninstalled_headers(self) -> None:
        root = self.make_tree(
            {
                "include/glifistore/a.hpp": '#include "glifistore/b.hpp"\n',
                "include/glifistore/b.hpp": '#include "glifistore/c.hpp"\n',
                "include/glifistore/c.hpp": "#pragma once\n",
            }
        )

        errors = header_closure_errors(root, ["include/glifistore/a.hpp"])

        self.assertEqual(len(errors), 2)
        self.assertTrue(any("does not install include/glifistore/b.hpp" in error for error in errors))
        self.assertTrue(any("does not install include/glifistore/c.hpp" in error for error in errors))

    def test_angle_bracket_project_header_counts_and_system_header_does_not(self) -> None:
        root = self.make_tree(
            {
                "include/glifistore/a.hpp": "#include <glifistore/b.hpp>\n#include <cstdint>\n",
                "include/glifistore/b.hpp": "#pragma once\n",
            }
        )

        errors = header_closure_errors(root, ["include/glifistore/a.hpp"])

        self.assertEqual(len(errors), 1)
        self.assertIn("include/glifistore/b.hpp", errors[0])

    def test_private_header_cannot_be_included_by_an_installed_header(self) -> None:
        root = self.make_tree(
            {
                "include/glifistore/a.hpp": '#include "glifistore/private_detail.hpp"\n',
                "src/glifistore/private_detail.hpp": "#pragma once\n",
            }
        )

        errors = header_closure_errors(root, ["include/glifistore/a.hpp"])

        self.assertEqual(len(errors), 1)
        self.assertIn("not a public header below include/", errors[0])

    def test_listed_header_must_exist(self) -> None:
        root = self.make_tree({})

        errors = header_closure_errors(root, ["include/glifistore/gone.hpp"])

        self.assertEqual(
            errors, ["install(FILES) lists include/glifistore/gone.hpp, which does not exist"]
        )

    def test_no_installed_headers_is_an_error_rather_than_a_pass(self) -> None:
        headers, layout_errors = installed_headers("install(TARGETS glifistore_core)\n")

        self.assertEqual((headers, layout_errors), ([], []))
        self.assertEqual(len(header_closure_errors(ROOT, headers)), 1)

    def test_install_destination_must_mirror_the_include_path(self) -> None:
        cmake_text = install_rule(
            "include/glifistore/store/blob.hpp", destination="glifistore/persistence"
        )

        headers, layout_errors = installed_headers(cmake_text)

        self.assertEqual(headers, ["include/glifistore/store/blob.hpp"])
        self.assertEqual(len(layout_errors), 1)
        self.assertIn("requires ${CMAKE_INSTALL_INCLUDEDIR}/glifistore/store", layout_errors[0])

    def test_rules_outside_the_include_directory_are_ignored(self) -> None:
        cmake_text = (
            'install(FILES "${CMAKE_CURRENT_BINARY_DIR}/x.pc"\n'
            '    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")\n'
            + install_rule("include/glifistore/a.hpp")
        )

        headers, layout_errors = installed_headers(cmake_text)

        self.assertEqual(headers, ["include/glifistore/a.hpp"])
        self.assertEqual(layout_errors, [])

    def test_commented_out_install_rules_do_not_count(self) -> None:
        cmake_text = (
            "# install(FILES include/glifistore/line.hpp\n"
            "#     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glifistore)\n"
            "#[[ install(FILES include/glifistore/bracket.hpp\n"
            "    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glifistore) ]]\n"
            + install_rule("include/glifistore/a.hpp").replace(
                "    COMPONENT", "    # trailing note )\n    COMPONENT"
            )
        )

        headers, layout_errors = installed_headers(cmake_text)

        self.assertEqual(headers, ["include/glifistore/a.hpp"])
        self.assertEqual(layout_errors, [])

    def test_install_command_name_is_case_insensitive(self) -> None:
        cmake_text = install_rule("include/glifistore/a.hpp").replace("install(", "INSTALL(")

        headers, layout_errors = installed_headers(cmake_text)

        self.assertEqual(headers, ["include/glifistore/a.hpp"])
        self.assertEqual(layout_errors, [])

    def test_hash_inside_a_quoted_argument_is_not_a_comment(self) -> None:
        cmake_text = install_rule("include/glifistore/a.hpp", destination="glifistore").replace(
            "    COMPONENT", '    RENAME "keep#this"\n    COMPONENT'
        )

        headers, layout_errors = installed_headers(cmake_text)

        self.assertEqual(headers, ["include/glifistore/a.hpp"])
        self.assertEqual(layout_errors, [])

    def test_package_inventories_must_list_exactly_the_installed_headers(self) -> None:
        payload = (
            "version: 1\ncomponents:\n  - id: development\n    entries:\n"
            '      - path: "@includedir@/glifistore/a.hpp"\n        kind: file\n'
            '      - path: "@libdir@/libglifistore_core.a"\n        kind: file\n'
        )
        root = self.make_tree(
            {
                "packaging/common/file-lists/payload.yaml": payload,
                "packaging/freebsd/pkg-plist": "bin/tool\ninclude/glifistore/a.hpp\n",
                "packaging/openbsd/pkg/PLIST": "@bin bin/tool\ninclude/glifistore/a.hpp\n",
            }
        )

        self.assertEqual(inventory_errors(root, ["include/glifistore/a.hpp"]), [])

        errors = inventory_errors(root, ["include/glifistore/a.hpp", "include/glifistore/b.hpp"])
        self.assertEqual(len(errors), 3)
        self.assertTrue(
            all("does not list installed header glifistore/b.hpp" in error for error in errors)
        )

        errors = inventory_errors(root, [])
        self.assertEqual(len(errors), 3)
        self.assertTrue(all("lists header glifistore/a.hpp" in error for error in errors))

    def test_missing_package_inventory_is_reported(self) -> None:
        root = self.make_tree({"packaging/freebsd/pkg-plist": "include/glifistore/a.hpp\n"})

        errors = inventory_errors(root, ["include/glifistore/a.hpp"])

        self.assertIn("missing package inventory: packaging/common/file-lists/payload.yaml", errors)
        self.assertIn("missing package inventory: packaging/openbsd/pkg/PLIST", errors)


if __name__ == "__main__":
    unittest.main()
