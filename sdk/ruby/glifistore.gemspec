# frozen_string_literal: true

Gem::Specification.new do |spec|
  spec.name          = "glifistore"
  spec.version       = begin
    require_relative "lib/glifi_store/version"
    GlifiStore::VERSION
  end
  spec.authors       = ["Giacomo Picchiarelli"]
  spec.email         = ["maintainers@glifistore.dev"]

  spec.summary       = "Official Ruby client for GlifiStore wire protocol v2"
  spec.description   = "Native isomorphic TCP client: pipelines, batch, structured errors, " \
                       "monotonic deadlines, and committed/rejected/indeterminate mutations."
  spec.homepage      = "https://github.com/gpicchiarelli/GlifiStore"
  spec.license       = "BSD-3-Clause"
  spec.required_ruby_version = ">= 3.2.0"

  spec.metadata["homepage_uri"] = spec.homepage
  spec.metadata["source_code_uri"] = "https://github.com/gpicchiarelli/GlifiStore/tree/main/sdk/ruby"
  spec.metadata["changelog_uri"] = "https://github.com/gpicchiarelli/GlifiStore/blob/main/sdk/ruby/CHANGELOG.md"
  spec.metadata["bug_tracker_uri"] = "https://github.com/gpicchiarelli/GlifiStore/issues"
  spec.metadata["documentation_uri"] = "https://github.com/gpicchiarelli/GlifiStore/blob/main/sdk/ruby/README.md"
  spec.metadata["allowed_push_host"] = "https://rubygems.org"
  spec.metadata["rubygems_mfa_required"] = "true"

  spec.files = Dir.chdir(__dir__) do
    tracked = begin
      `git ls-files -z`.split("\x0")
    rescue StandardError
      []
    end
    selected = tracked.select do |f|
      f.start_with?("lib/", "exe/") ||
        %w[LICENSE NOTICE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md PACKAGING.md].include?(f)
    end
    # Always ship copyright notices even if git has not yet indexed new notice files.
    %w[LICENSE NOTICE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md PACKAGING.md].each do |name|
      selected << name if File.file?(name) && !selected.include?(name)
    end
    selected.concat(Dir["lib/**/*", "exe/*"]).uniq
  end
  spec.bindir = "exe"
  spec.executables = spec.files.grep(%r{\Aexe/}) { |f| File.basename(f) }
  spec.require_paths = ["lib"]

  # Optional Fiber client: require "glifi_store/async_client"
  spec.add_development_dependency "async", "~> 2.0"
  spec.add_development_dependency "minitest", "~> 5.0"
end
