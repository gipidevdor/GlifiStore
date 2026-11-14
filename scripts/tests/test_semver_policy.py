from __future__ import annotations

import unittest

from engineering.tools.semver_policy import (
    BACKENDS,
    SemverError,
    normalize,
    normalize_all,
    parse,
    release_kind,
    select_previous,
    sort_versions,
)


class ParsingTests(unittest.TestCase):
    def test_parses_core_prerelease_and_build_metadata(self) -> None:
        version = parse("1.2.3-rc.1+build.7")
        self.assertEqual((version.major, version.minor, version.patch), (1, 2, 3))
        self.assertEqual(version.prerelease, ("rc", "1"))
        self.assertEqual(version.build, ("build", "7"))
        self.assertTrue(version.is_prerelease)
        self.assertEqual(str(version), "1.2.3-rc.1+build.7")

    def test_accepts_the_tag_prefix_only_when_allowed(self) -> None:
        self.assertEqual(str(parse("v0.1.0")), "0.1.0")
        with self.assertRaises(SemverError):
            parse("v0.1.0", allow_v_prefix=False)

    def test_rejects_versions_that_are_not_strict_semver(self) -> None:
        for candidate in ("1.2", "1.02.3", "01.2.3", "1.2.3.4", "1.2.3-", "1.2.3+", "", "next"):
            with self.subTest(candidate=candidate), self.assertRaises(SemverError):
                parse(candidate)


class OrderingTests(unittest.TestCase):
    def test_follows_semver_precedence_for_prereleases(self) -> None:
        ordered = [
            "1.0.0-alpha",
            "1.0.0-alpha.1",
            "1.0.0-alpha.beta",
            "1.0.0-beta",
            "1.0.0-beta.2",
            "1.0.0-beta.11",
            "1.0.0-rc.1",
            "1.0.0",
        ]
        shuffled = [ordered[index] for index in (7, 2, 5, 0, 6, 4, 1, 3)]
        self.assertEqual([str(version) for version in sort_versions(shuffled)], ordered)

    def test_orders_numerically_not_lexically(self) -> None:
        self.assertEqual(
            [str(version) for version in sort_versions(["0.10.0", "0.9.0", "0.2.11"])],
            ["0.2.11", "0.9.0", "0.10.0"],
        )

    def test_build_metadata_does_not_affect_precedence(self) -> None:
        self.assertFalse(parse("1.0.0+a") < parse("1.0.0+b"))
        self.assertFalse(parse("1.0.0+b") < parse("1.0.0+a"))


class ReleaseKindTests(unittest.TestCase):
    def test_classifies_the_change_against_the_previous_release(self) -> None:
        self.assertEqual(release_kind("0.1.0"), "initial")
        self.assertEqual(release_kind("1.0.0", "0.9.3"), "major")
        self.assertEqual(release_kind("0.2.0", "0.1.7"), "minor")
        self.assertEqual(release_kind("0.1.8", "0.1.7"), "patch")
        self.assertEqual(release_kind("0.2.0-rc.1", "0.1.7"), "prerelease")

    def test_refuses_a_version_that_does_not_move_forward(self) -> None:
        for current, previous in (("0.1.0", "0.1.0"), ("0.1.0", "0.2.0"), ("1.0.0-rc.1", "1.0.0")):
            with self.subTest(current=current), self.assertRaises(SemverError):
                release_kind(current, previous)


class PreviousSelectionTests(unittest.TestCase):
    def test_selects_the_greatest_older_release_not_current_minus_one(self) -> None:
        candidates = ["0.9.0", "0.10.0", "0.2.11", "2.0.0"]
        self.assertEqual(str(select_previous(candidates, "1.0.0")), "0.10.0")

    def test_first_release_has_no_previous(self) -> None:
        self.assertIsNone(select_previous([], "0.1.0"))
        self.assertIsNone(select_previous(["0.1.0", "1.0.0"], "0.1.0"))

    def test_prereleases_are_excluded_unless_requested(self) -> None:
        candidates = ["1.0.0-rc.1", "0.9.0"]
        self.assertEqual(str(select_previous(candidates, "1.0.0")), "0.9.0")
        self.assertEqual(
            str(select_previous(candidates, "1.0.0", include_prereleases=True)), "1.0.0-rc.1"
        )


class NormalizationTests(unittest.TestCase):
    def test_covers_every_declared_backend(self) -> None:
        packages = normalize_all("0.1.0")
        self.assertEqual(sorted(packages), sorted(BACKENDS))
        self.assertTrue(all(package.upstream_version for package in packages.values()))

    def test_release_versions_map_to_native_grammars(self) -> None:
        expected = {
            "deb": "0.1.0-1",
            "rpm": "0.1.0-1",
            "macports": "0.1.0_0",
            "homebrew": "0.1.0",
            "freebsd": "0.1.0",
            "openbsd": "0.1.0",
        }
        for backend, package_version in expected.items():
            with self.subTest(backend=backend):
                self.assertEqual(normalize("0.1.0", backend).package_version, package_version)

    def test_package_revision_is_packaging_only_and_never_touches_the_product_version(self) -> None:
        expected = {
            "deb": ("0.1.0", "0.1.0-3"),
            "rpm": ("0.1.0", "0.1.0-3"),
            "macports": ("0.1.0", "0.1.0_2"),
            "homebrew": ("0.1.0", "0.1.0_2"),
            "freebsd": ("0.1.0", "0.1.0_2"),
            "openbsd": ("0.1.0", "0.1.0p2"),
        }
        for backend, (upstream, package_version) in expected.items():
            with self.subTest(backend=backend):
                package = normalize("0.1.0", backend, 2)
                self.assertEqual(package.upstream_version, upstream)
                self.assertEqual(package.package_version, package_version)

    def test_debian_and_rpm_sort_prereleases_before_the_release(self) -> None:
        for backend in ("deb", "rpm"):
            with self.subTest(backend=backend):
                self.assertEqual(normalize("1.2.3-rc.1", backend).upstream_version, "1.2.3~rc.1")

    def test_backends_without_a_tilde_grammar_declare_their_ordering_limitation(self) -> None:
        for backend in ("macports", "homebrew", "freebsd", "openbsd"):
            with self.subTest(backend=backend):
                self.assertTrue(normalize("1.2.3-rc.1", backend).limitations)

    def test_prerelease_versions_stay_inside_each_native_grammar(self) -> None:
        expected = {
            "deb": "1.2.3~rc.1-1",
            "rpm": "1.2.3~rc.1-1",
            "macports": "1.2.3.rc.1_0",
            "homebrew": "1.2.3-rc.1",
            "freebsd": "1.2.3.rc.1",
            "openbsd": "1.2.3rc1",
        }
        for backend, package_version in expected.items():
            with self.subTest(backend=backend):
                self.assertEqual(normalize("1.2.3-rc.1", backend).package_version, package_version)

    def test_build_metadata_is_dropped_with_an_explicit_limitation(self) -> None:
        package = normalize("1.2.3+build.7", "deb")
        self.assertEqual(package.package_version, "1.2.3-1")
        self.assertTrue(any("build metadata" in item for item in package.limitations))

    def test_refuses_unknown_backends_and_invalid_revisions(self) -> None:
        with self.assertRaises(SemverError):
            normalize("0.1.0", "windows")
        with self.assertRaises(SemverError):
            normalize("0.1.0", "deb", -1)


if __name__ == "__main__":
    unittest.main()
