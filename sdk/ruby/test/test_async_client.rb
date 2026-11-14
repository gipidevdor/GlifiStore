# frozen_string_literal: true

require "minitest/autorun"
require "glifi_store"
require "glifi_store/async_client"
require_relative "fake_server"

class AsyncClientTest < Minitest::Test
  def test_async_put_get_pipeline
    server = FakeServer.new
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
      key = "async\x00key".b
      value = "payload".b
      assert client.put(key, value).committed?
      assert_equal value, client.get(key)
      responses = client.execute_pipeline(
        [
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::PUT, key: key, value: "v2".b),
          GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: key)
        ]
      )
      assert responses[1].succeeded?
      assert_equal "v2".b, responses[1].value
      client.close
    end
  ensure
    server&.join
  end

  def test_async_erase_absent_is_rejected
    # client-semantics §3: wire NOT_FOUND on standalone ERASE → rejected.
    server = FakeServer.new
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
      result = client.erase("missing-key".b)
      assert result.rejected?
      assert_equal GlifiStore::Category::NOT_FOUND, result.error.category
      assert_equal GlifiStore::MutationOutcome::REJECTED, result.error.mutation_outcome
      assert_equal GlifiStore::Retryability::NEW_ATTEMPT, result.error.retryability
      client.close
    end
  ensure
    server&.join
  end

  def test_async_pipeline_erase_absent_is_failed_rejected
    # client-semantics §3: wire NOT_FOUND on pipeline ERASE → failed + rejected.
    server = FakeServer.new
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
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
    end
  ensure
    server&.join
  end

  def test_async_batch_erase_absent_is_failed_rejected
    # client-semantics §3/§5: execute_batch absent ERASE → failed + rejected.
    server = FakeServer.new
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
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
    end
  ensure
    server&.join
  end

  def test_async_worker_pipelines_erase_absent_is_failed_rejected
    # client-semantics §3/§5: pre-sharded worker pipelines inherit pipeline ERASE mapping.
    server = FakeServer.new
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
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
    end
  ensure
    server&.join
  end

  def test_async_batch_two_workers
    server = FakeServer.new(workers: 2)
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
      keys = [nil, nil]
      candidate = 0
      while keys.any?(&:nil?)
        key = format("ak%08d", candidate).b
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
      assert_equal "a".b, responses[2].value
      assert_equal "b".b, responses[3].value
      client.close
    end
  ensure
    server&.join
  end

  def test_async_worker_pipelines_pre_sharded_fanout
    server = FakeServer.new(workers: 2)
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
      keys = [nil, nil]
      candidate = 0
      while keys.any?(&:nil?)
        key = format("awp%08d", candidate).b
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
    end
  ensure
    server&.join
  end

  def test_async_cancel_poisons_connection
    server = FakeServer.new
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
      task = Async do
        client.get("missing".b)
      end
      task.stop
      begin
        task.wait
      rescue Async::Stop
        # expected for non-mutation cancel
      end
      err = assert_raises(GlifiStore::Error) { client.get("missing".b) }
      assert_equal GlifiStore::Category::NOT_FOUND, err.category
      client.close
    end
  ensure
    server&.join
  end

  def test_async_cancel_after_put_send_is_indeterminate
    server = FakeServer.new(stall_on_put: true)
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      config.request_timeout = 5.0
      client = GlifiStore::AsyncClient.connect(config)
      result = nil
      task = Async do
        result = client.put("key".b, "value".b)
      end
      # Allow the PUT frame to leave the client and block on receive.
      Async::Task.current.sleep(0.05)
      task.stop
      task.wait
      assert result
      assert_equal GlifiStore::MutationOutcome::INDETERMINATE, result.outcome
      assert result.error
      assert_operator result.error.bytes_sent, :>, 0
      assert_equal GlifiStore::Retryability::RECONCILE_FIRST, result.error.retryability
      assert_equal GlifiStore::MutationOutcome::INDETERMINATE, result.error.mutation_outcome
      # Poisoned Worker connection must still admit a fresh GET after cancel.
      err = assert_raises(GlifiStore::Error) { client.get("missing".b) }
      assert_equal GlifiStore::Category::NOT_FOUND, err.category
      client.close
    end
  ensure
    server&.join
  end

  def test_async_pipeline_cancel_after_send_classifies_mutations
    server = FakeServer.new(stall_on_put: true)
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      config.request_timeout = 5.0
      client = GlifiStore::AsyncClient.connect(config)
      responses = nil
      task = Async do
        responses = client.execute_pipeline(
          [
            GlifiStore::PipelineRequest.new(
              opcode: GlifiStore::PipelineOpcode::PUT, key: "k".b, value: "v".b
            ),
            GlifiStore::PipelineRequest.new(opcode: GlifiStore::PipelineOpcode::GET, key: "k".b)
          ]
        )
      end
      Async::Task.current.sleep(0.05)
      task.stop
      task.wait
      assert responses
      assert_equal GlifiStore::PipelineOutcome::INDETERMINATE, responses[0].outcome
      assert responses[0].error
      assert_operator responses[0].error.bytes_sent, :>, 0
      assert_equal GlifiStore::Retryability::RECONCILE_FIRST, responses[0].error.retryability
      assert_equal GlifiStore::MutationOutcome::INDETERMINATE, responses[0].error.mutation_outcome
      assert_equal GlifiStore::PipelineOutcome::FAILED, responses[1].outcome
      client.close
    end
  ensure
    server&.join
  end

  def test_async_batch_cancel_preserves_sibling_results
    # Worker-0 PUT succeeds; Worker-1 stalls. Outer cancel must return the slot
    # vector (sibling committed + stalled indeterminate), not bare Async::Stop.
    server = FakeServer.new(workers: 2, stall_on_put_workers: [1])
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      config.request_timeout = 5.0
      client = GlifiStore::AsyncClient.connect(config)
      keys = [nil, nil]
      candidate = 0
      while keys.any?(&:nil?)
        key = format("bcancel%08d", candidate).b
        w = client.worker_for(key)
        keys[w] ||= key
        candidate += 1
      end
      responses = nil
      task = Async do
        responses = client.execute_batch(
          [
            GlifiStore::PipelineRequest.new(
              opcode: GlifiStore::PipelineOpcode::PUT, key: keys[0], value: "a".b
            ),
            GlifiStore::PipelineRequest.new(
              opcode: GlifiStore::PipelineOpcode::PUT, key: keys[1], value: "b".b
            )
          ]
        )
      end
      Async::Task.current.sleep(0.05)
      task.stop
      task.wait
      assert responses
      assert_equal 2, responses.length
      assert_equal GlifiStore::PipelineOutcome::SUCCEEDED, responses[0].outcome
      assert_equal GlifiStore::PipelineOutcome::INDETERMINATE, responses[1].outcome
      assert responses[1].error
      assert_operator responses[1].error.bytes_sent, :>, 0
      assert_equal GlifiStore::Retryability::RECONCILE_FIRST, responses[1].error.retryability
      assert_equal GlifiStore::MutationOutcome::INDETERMINATE, responses[1].error.mutation_outcome
      assert_equal "a".b, client.get(keys[0])
      client.close
    end
  ensure
    server&.join
  end

  def test_async_internal_error_mutation_stamps_bytes_sent
    server = FakeServer.new(internal_error_on_put: true)
    Async do
      config = GlifiStore::ClientConfig.defaults
      config.port = server.port
      client = GlifiStore::AsyncClient.connect(config)
      result = client.put("key".b, "value".b)
      assert result.indeterminate?
      assert_equal GlifiStore::Retryability::RECONCILE_FIRST, result.error.retryability
      assert_equal GlifiStore::MutationOutcome::INDETERMINATE, result.error.mutation_outcome
      assert_operator result.error.bytes_sent, :>, 0
      client.close
    end
  ensure
    server&.join
  end
end
