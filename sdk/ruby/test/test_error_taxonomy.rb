# frozen_string_literal: true

require "json"
require "pathname"
require "minitest/autorun"
require "glifi_store"

class TestErrorTaxonomy < Minitest::Test
  FIXTURES = Pathname.new(__dir__).join("fixtures")
  REPO_FIXTURES = Pathname.new(__dir__).join("../../../tests/fixtures")

  def fixture(name)
    path = FIXTURES.join(name)
    path = REPO_FIXTURES.join(name) unless path.file?
    JSON.parse(path.read)
  end

  def test_wire_status_category_retryability_matrix
    fixture("error_taxonomy_v1.json").fetch("cases").each do |case_item|
      error = GlifiStore::Error.from_status(case_item.fetch("wire_status"))
      assert_equal case_item.fetch("category"), error.category, case_item["id"]
      assert_equal case_item.fetch("wire_status"), error.wire_status, case_item["id"]
      assert_equal case_item.fetch("read_retryability"), error.retryability, case_item["id"]

      enriched = GlifiStore::Error.from_status(case_item.fetch("wire_status")).enrich(
        bytes_sent: 1,
        mutation_outcome: case_item.fetch("mutation_outcome")
      )
      assert_equal case_item.fetch("mutation_retryability"), enriched.retryability, case_item["id"]

      want_unhealthy =
        case_item.fetch("wire_status") == GlifiStore::Protocol::Status::WRONG_OWNER ||
        case_item.fetch("wire_status") == GlifiStore::Protocol::Status::NOT_BOUND
      assert_equal want_unhealthy, case_item.fetch("unhealthy"), case_item["id"]
    end
  end

  def test_indeterminate_enrich_ignores_zero_bytes_sent
    # Receive-after-send paths historically omitted bytes_sent; transport+0 must not
    # advertise same_request when mutation_outcome is already indeterminate.
    error = GlifiStore::Error.transport("socket closed")
                              .enrich(mutation_outcome: GlifiStore::MutationOutcome::INDETERMINATE)
    assert_equal 0, error.bytes_sent
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, error.retryability
    assert_equal GlifiStore::MutationOutcome::INDETERMINATE, error.mutation_outcome
  end
end
