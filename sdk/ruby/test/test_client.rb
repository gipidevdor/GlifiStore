# frozen_string_literal: true

require "minitest/autorun"
require "glifi_store"
require_relative "fake_server"

class ClientTest < Minitest::Test
  def test_put_get_ping_erase_and_structured_errors
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    key = "binary\x00key".b
    value = "value\x00\xff".b
    assert client.put(key, value).committed?
    assert_equal value, client.get(key)
    assert_equal "hello".b, client.ping("hello".b)
    assert client.erase(key).committed?
    err = assert_raises(GlifiStore::Error) { client.get(key) }
    assert_equal GlifiStore::Category::NOT_FOUND, err.category
    assert_equal GlifiStore::Protocol::Status::NOT_FOUND, err.wire_status
    assert_equal GlifiStore::Retryability::NEW_ATTEMPT, err.retryability
    assert_equal "get", err.operation
    client.close
  ensure
    server&.join
  end

  def test_erase_absent_is_rejected
    # client-semantics §3: wire NOT_FOUND on standalone ERASE → rejected.
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    result = client.erase("missing-key".b)
    assert result.rejected?
    assert_equal GlifiStore::Category::NOT_FOUND, result.error.category
    assert_equal GlifiStore::MutationOutcome::REJECTED, result.error.mutation_outcome
    assert_equal GlifiStore::Retryability::NEW_ATTEMPT, result.error.retryability
    client.close
  ensure
    server&.join
  end

  def test_pipeline_erase_absent_is_failed_rejected
    # client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    responses = client.execute_pipeline(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::ERASE, key: "missing-key".b)
      ]
    )
    assert_equal 1, responses.length
    assert_equal GlifiStore::PipelineOutcome::FAILED, responses[0].outcome
    assert responses[0].error
    assert_equal GlifiStore::Category::NOT_FOUND, responses[0].error.category
    assert_equal GlifiStore::MutationOutcome::REJECTED, responses[0].error.mutation_outcome
    assert_equal GlifiStore::Retryability::NEW_ATTEMPT, responses[0].error.retryability
    client.close
  ensure
    server&.join
  end

  def test_batch_erase_absent_is_failed_rejected
    # client-semantics §3/§5: execute_batch absent ERASE → failed + rejected.
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    responses = client.execute_batch(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::ERASE, key: "missing-key".b)
      ]
    )
    assert_equal 1, responses.length
    assert_equal GlifiStore::PipelineOutcome::FAILED, responses[0].outcome
    assert responses[0].error
    assert_equal GlifiStore::Category::NOT_FOUND, responses[0].error.category
    assert_equal GlifiStore::MutationOutcome::REJECTED, responses[0].error.mutation_outcome
    assert_equal GlifiStore::Retryability::NEW_ATTEMPT, responses[0].error.retryability
    client.close
  ensure
    server&.join
  end

  def test_worker_pipelines_erase_absent_is_failed_rejected
    # client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    results = client.execute_worker_pipelines(
      [
        [
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::ERASE, key: "missing-key".b)
        ]
      ]
    )
    assert_equal 1, results.length
    assert_equal 1, results[0].length
    assert_equal GlifiStore::PipelineOutcome::FAILED, results[0][0].outcome
    assert results[0][0].error
    assert_equal GlifiStore::Category::NOT_FOUND, results[0][0].error.category
    assert_equal GlifiStore::MutationOutcome::REJECTED, results[0][0].error.mutation_outcome
    assert_equal GlifiStore::Retryability::NEW_ATTEMPT, results[0][0].error.retryability
    client.close
  ensure
    server&.join
  end

  def test_health_ready_stats_and_routing
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    assert client.healthy?
    refute_nil client.routing
    assert_equal "GlifiStore/live".b, client.health
    assert_equal "GlifiStore/ready".b, client.ready
    stats = client.stats
    assert stats.start_with?("GlifiStore/stats".b)
    client.close
  ensure
    server&.join
  end

  def test_unix_socket_refuses_tls
    err = assert_raises(GlifiStore::Error) do
      GlifiStore::Client.connect(
        GlifiStore::ClientConfig.defaults.tap do |c|
          c.unix_socket_path = "/tmp/glifistore-rb-uds-tls-refuse.sock"
          c.tls = true
        end
      )
    end
    assert_equal GlifiStore::Category::INVALID_ARGUMENT, err.category
    assert_match(/AF_UNIX/, err.message)
  end

  def test_internal_error_mutation_is_indeterminate
    server = FakeServer.new(internal_error_on_put: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    result = client.put("key".b, "value".b)
    assert result.indeterminate?
    assert_equal GlifiStore::Category::INTERNAL, result.error.category
    assert_equal GlifiStore::Protocol::Status::INTERNAL_ERROR, result.error.wire_status
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, result.error.retryability
    assert_equal "put", result.error.operation
    assert_operator result.error.bytes_sent, :>, 0
    client.close
  ensure
    server&.join
  end

  def test_backup_internal_error_is_reconcile_first
    server = FakeServer.new(internal_error_on_backup: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    err = assert_raises(GlifiStore::Error) do
      client.backup("/tmp/glifistore-ruby-backup-internal")
    end
    assert_equal GlifiStore::MutationOutcome::INDETERMINATE, err.mutation_outcome
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, err.retryability
    assert_equal GlifiStore::Protocol::Status::INTERNAL_ERROR, err.wire_status
    assert_equal 1, server.backup_requests
    client.close
  ensure
    server&.join
  end

  def test_backup_validate_failure_is_reconcile_first
    server = FakeServer.new(wrong_request_id_on_backup: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    err = assert_raises(GlifiStore::Error) do
      client.backup("/tmp/glifistore-ruby-backup-wrong-id")
    end
    assert_equal GlifiStore::MutationOutcome::INDETERMINATE, err.mutation_outcome
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, err.retryability
    assert_operator err.bytes_sent, :>, 0
    assert_equal 1, server.backup_requests
    client.close
  ensure
    server&.join
  end

  def test_permission_denied_status_eight
    server = FakeServer.new(deny_data_plane: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    err = assert_raises(GlifiStore::Error) { client.get("key".b) }
    assert_equal GlifiStore::Category::PERMISSION_DENIED, err.category
    assert_equal GlifiStore::Protocol::Status::PERMISSION_DENIED, err.wire_status
    assert_equal GlifiStore::Retryability::NEVER, err.retryability
    result = client.put("key".b, "value".b)
    assert result.rejected?
    assert_equal GlifiStore::Category::PERMISSION_DENIED, result.error.category
    assert_equal GlifiStore::Protocol::Status::PERMISSION_DENIED, result.error.wire_status
    client.close
  ensure
    server&.join
  end

  def test_overloaded_retryability_is_never
    err = GlifiStore::Error.overloaded("server is overloaded")
    assert_equal GlifiStore::Category::OVERLOADED, err.category
    assert_equal GlifiStore::Retryability::NEVER, err.retryability
    assert_equal GlifiStore::Retryability::NEVER,
                 GlifiStore::Error.retryability_for(GlifiStore::Category::OVERLOADED, false, false)
  end

  def test_pipeline_put_get
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    key = "pipe".b
    value = "v".b
    responses = client.execute_pipeline(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: key, value: value),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: key)
      ]
    )
    assert_equal 2, responses.length
    assert responses[0].succeeded?
    assert responses[1].succeeded?
    assert_equal value, responses[1].value
    client.close
  ensure
    server&.join
  end

  def test_batch_groups_workers
    server = FakeServer.new(workers: 2)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    assert_equal 2, client.worker_count
    # Find keys for each worker
    keys = [nil, nil]
    candidate = 0
    while keys.any?(&:nil?)
      key = format("k%08d", candidate).b
      w = client.worker_for(key)
      keys[w] ||= key
      candidate += 1
    end
    responses = client.execute_batch(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: keys[0], value: "a".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: keys[1], value: "b".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[0]),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[1])
      ]
    )
    assert responses[0].succeeded?
    assert responses[1].succeeded?
    assert_equal "a".b, responses[2].value
    assert_equal "b".b, responses[3].value
    client.close
  ensure
    server&.join
  end

  def test_worker_pipelines_pre_sharded_fanout
    server = FakeServer.new(workers: 2)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    keys = [nil, nil]
    candidate = 0
    while keys.any?(&:nil?)
      key = format("wp%08d", candidate).b
      w = client.worker_for(key)
      keys[w] ||= key
      candidate += 1
    end
    results = client.execute_worker_pipelines(
      [
        [
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: keys[0], value: "a".b),
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[0])
        ],
        [
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: keys[1], value: "b".b),
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[1])
        ]
      ]
    )
    assert_equal 2, results.length
    assert results[0][0].succeeded?
    assert_equal "a".b, results[0][1].value
    assert results[1][0].succeeded?
    assert_equal "b".b, results[1][1].value
    sparse = client.execute_worker_pipelines(
      [
        [],
        [GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[1])]
      ]
    )
    assert_equal [], sparse[0]
    assert sparse[1][0].succeeded?
    err = assert_raises(GlifiStore::Error) do
      client.execute_worker_pipelines(
        [
          [GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[1])],
          []
        ]
      )
    end
    assert_equal GlifiStore::Category::INVALID_ARGUMENT, err.category
    err = assert_raises(GlifiStore::Error) do
      client.execute_worker_pipelines(
        [[GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: keys[0])]]
      )
    end
    assert_equal GlifiStore::Category::INVALID_ARGUMENT, err.category
    client.close
  ensure
    server&.join
  end

  def test_batch_preserves_sibling_results_when_one_worker_fails
    server = FakeServer.new(workers: 2, fail_rebind_workers: [1])
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    keys = [nil, nil]
    candidate = 0
    while keys.any?(&:nil?)
      key = format("sib%08d", candidate).b
      w = client.worker_for(key)
      keys[w] ||= key
      candidate += 1
    end
    client.instance_variable_get(:@connections)[1].reset!
    responses = client.execute_batch(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: keys[0], value: "a".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: keys[1], value: "b".b)
      ]
    )
    assert_equal 2, responses.length
    assert responses[0].succeeded?
    assert_equal GlifiStore::PipelineOutcome::FAILED, responses[1].outcome
    assert responses[1].error
    assert_equal GlifiStore::MutationOutcome::REJECTED, responses[1].error.mutation_outcome
    assert_equal 0, responses[1].error.bytes_sent
    assert_equal "a".b, client.get(keys[0])
    client.close
  ensure
    server&.join
  end

  def test_rejects_non_positive_timeout
    server = FakeServer.new
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    err = assert_raises(GlifiStore::Error) { client.get("k".b, timeout: 0) }
    assert_equal GlifiStore::Category::INVALID_ARGUMENT, err.category
    client.close
  ensure
    server&.join
  end

  def test_pipeline_disconnect_classifies_mutations
    server = FakeServer.new(drop_after_mutation: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    responses = client.execute_pipeline(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: "k".b, value: "v".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: "k".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::ERASE, key: "k".b)
      ]
    )
    # After PUT succeeds and server drops, GET/ERASE unresolved; if drop happens after PUT reply,
    # first may succeed. Fake drops after writing PUT OK — subsequent receive fails.
    assert_equal GlifiStore::PipelineOutcome::SUCCEEDED, responses[0].outcome
    assert_equal GlifiStore::PipelineOutcome::FAILED, responses[1].outcome
    assert_equal GlifiStore::PipelineOutcome::INDETERMINATE, responses[2].outcome
    assert responses[2].error
    assert_operator responses[2].error.bytes_sent, :>, 0
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, responses[2].error.retryability
    assert_equal GlifiStore::MutationOutcome::INDETERMINATE, responses[2].error.mutation_outcome
    client.close
  ensure
    server&.join
  end

  def test_pipeline_disconnect_before_put_reply_is_reconcile_first
    server = FakeServer.new(disconnect_on_put: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    responses = client.execute_pipeline(
      [
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: "k".b, value: "v".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: "k".b),
        GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::ERASE, key: "k".b)
      ]
    )
    assert_equal GlifiStore::PipelineOutcome::INDETERMINATE, responses[0].outcome
    assert_equal GlifiStore::PipelineOutcome::FAILED, responses[1].outcome
    assert_equal GlifiStore::PipelineOutcome::INDETERMINATE, responses[2].outcome
    assert responses[0].error
    assert_operator responses[0].error.bytes_sent, :>, 0
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, responses[0].error.retryability
    assert_equal GlifiStore::MutationOutcome::INDETERMINATE, responses[0].error.mutation_outcome
    client.close
  ensure
    server&.join
  end

  def test_pipeline_internal_error_mutation_is_reconcile_first
    server = FakeServer.new(internal_error_on_put: true)
    client = GlifiStore::Client.connect(
      GlifiStore::ClientConfig.defaults.tap { |c| c.port = server.port }
    )
    responses = client.execute_pipeline(
      [GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: "k".b, value: "v".b)]
    )
    assert_equal GlifiStore::PipelineOutcome::INDETERMINATE, responses[0].outcome
    assert responses[0].error
    assert_equal GlifiStore::Retryability::RECONCILE_FIRST, responses[0].error.retryability
    assert_equal GlifiStore::MutationOutcome::INDETERMINATE, responses[0].error.mutation_outcome
    assert_equal GlifiStore::Protocol::Status::INTERNAL_ERROR, responses[0].error.wire_status
    assert_operator responses[0].error.bytes_sent, :>, 0
    client.close
  ensure
    server&.join
  end

  def test_tls_config_requires_cert_and_key_pair
    err = assert_raises(GlifiStore::Error) do
      GlifiStore::Client.connect(
        GlifiStore::ClientConfig.defaults.tap do |c|
          c.port = 1
          c.tls = true
          c.cert_file = "only-cert.pem"
        end
      )
    end
    assert_equal GlifiStore::Category::INVALID_ARGUMENT, err.category
  end

  def test_build_ssl_context_requires_tls_1_3
    skip "TLS 1.3 unavailable in this Ruby/OpenSSL" unless GlifiStore::Tls.tls13_available?

    context = GlifiStore::Tls.build_ssl_context(
      GlifiStore::ClientConfig.defaults.tap do |c|
        c.tls = true
        c.insecure_skip_verify = true
      end
    )
    if context.respond_to?(:min_version) && context.respond_to?(:max_version)
      assert_equal OpenSSL::SSL::TLS1_3_VERSION, context.min_version
      assert_equal OpenSSL::SSL::TLS1_3_VERSION, context.max_version
    end
    assert_equal OpenSSL::SSL::VERIFY_NONE, context.verify_mode
  end

  def test_sync_and_async_tls_ping
    skip "TLS 1.3 unavailable in this Ruby/OpenSSL" unless GlifiStore::Tls.tls13_available?

    material = self_signed_material
    skip "openssl CLI unavailable for ephemeral certs" if material.nil?

    cert_path, key_path = material
    server_context = OpenSSL::SSL::SSLContext.new
    if server_context.respond_to?(:min_version=) && server_context.respond_to?(:max_version=)
      server_context.min_version = OpenSSL::SSL::TLS1_3_VERSION
      server_context.max_version = OpenSSL::SSL::TLS1_3_VERSION
    elsif server_context.respond_to?(:ssl_version=)
      server_context.ssl_version = :TLSv1_3
    else
      skip "OpenSSL::SSL::SSLContext cannot pin TLS 1.3 on this Ruby"
    end
    server_context.cert = OpenSSL::X509::Certificate.new(File.binread(cert_path))
    server_context.key = OpenSSL::PKey.read(File.binread(key_path))
    server = FakeServer.new(ssl_context: server_context)
    config = GlifiStore::ClientConfig.defaults.tap do |c|
      c.port = server.port
      c.tls = true
      c.tls_ca = cert_path
      c.server_name = "localhost"
    end
    client = GlifiStore::Client.connect(config)
    assert_equal "tls-ping".b, client.ping("tls-ping".b)
    client.close

    require "glifi_store/async_client"
    Async do
      async_client = GlifiStore::AsyncClient.connect(config)
      assert_equal "tls-async".b, async_client.ping("tls-async".b)
      async_client.close
    end
  ensure
    server&.join
  end

  def self_signed_material
    require "tmpdir"
    require "open3"
    directory = Dir.mktmpdir("glifistore-rb-tls-")
    cert_path = File.join(directory, "server.crt")
    key_path = File.join(directory, "server.key")
    _out, _err, status = Open3.capture3(
      "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
      "-keyout", key_path, "-out", cert_path, "-days", "1", "-subj", "/CN=localhost"
    )
    return nil unless status.success?

    [cert_path, key_path]
  rescue Errno::ENOENT
    nil
  end
  def test_keyed_siphash_init_and_routing
    server = FakeServer.new(workers: 8)
    server.routing = GlifiStore::Protocol::WorkerRouting.new(
      algorithm: GlifiStore::Protocol::ROUTING_ALG_SIPHASH24_V1,
      seed: 0x1111222233334444
    )
    client = GlifiStore::Client.connect(GlifiStore::ClientConfig.new(port: server.port))
    begin
      assert client.routing.keyed?
      assert_equal 0x1111222233334444, client.routing.seed
      assert_equal 6, client.worker_for("tenant-a/orders/1".b)
      assert client.put("tenant-a/orders/1".b, "v".b).committed?
      assert_equal "v".b, client.get("tenant-a/orders/1".b)
    ensure
      client.close
      server&.join
    end
  end

end
