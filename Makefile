# Thin convenience wrapper: every target delegates to scripts/dev.sh, which is the
# single source of truth for presets and tools. Run `make help` for details.
DEV := ./scripts/dev.sh
DEV_COMMANDS := doctor configure build test format format-check verify clean \
	asan tsan test-lto correctness fuzz-build fuzz-run \
	benchmark benchmark-durable benchmark-compaction benchmark-maintenance \
	benchmark-server benchmark-lto benchmark-pgo pgo-generate pgo-train pgo-use xcode-build

.DEFAULT_GOAL := help
.PHONY: help bootstrap xcode xcode-verify $(DEV_COMMANDS)

help:
	@$(DEV) help
	@echo
	@echo "make targets mirror these commands; also: bootstrap, xcode, xcode-verify (macOS)."

bootstrap:
	./scripts/bootstrap-macos.sh

xcode:
	./scripts/open-xcode.sh

xcode-verify:
	./scripts/verify-xcode.sh

$(DEV_COMMANDS):
	$(DEV) $@
