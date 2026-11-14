# frozen_string_literal: true

module GlifiStore
  # Structured client failure (client-semantics v1 §2.1).
  class Error < StandardError
    attr_reader :category, :wire_status, :bytes_sent, :request_id, :worker,
                :routing_epoch, :retryability, :operation, :mutation_outcome

    def initialize(
      category:,
      message:,
      wire_status: nil,
      bytes_sent: 0,
      request_id: nil,
      worker: nil,
      routing_epoch: nil,
      retryability: nil,
      operation: nil,
      mutation_outcome: nil
    )
      @category = category
      @wire_status = wire_status
      @bytes_sent = bytes_sent
      @request_id = request_id
      @worker = worker
      @routing_epoch = routing_epoch
      @operation = operation
      @mutation_outcome = mutation_outcome
      @retryability = retryability || self.class.retryability_for(
        category,
        bytes_sent.positive? && !mutation_outcome.nil?,
        mutation_outcome == MutationOutcome::INDETERMINATE
      )
      super(message)
    end

    def self.retryability_for(category, mutation_sent, indeterminate)
      return Retryability::RECONCILE_FIRST if indeterminate
      return Retryability::NEVER if category == Category::INVALID_ARGUMENT && !mutation_sent
      return Retryability::SAME_REQUEST if category == Category::TRANSPORT && !mutation_sent
      return Retryability::NEVER if category == Category::OVERLOADED
      return Retryability::NEVER if category == Category::PERMISSION_DENIED
      return Retryability::NEW_ATTEMPT if category == Category::NOT_FOUND
      return Retryability::NEVER if category == Category::UNAVAILABLE
      return Retryability::RECONCILE_FIRST if mutation_sent

      Retryability::NEW_ATTEMPT
    end

    def base_copy
      self.class.new(category: @category, message: message, wire_status: @wire_status)
    end

    def enrich(
      wire_status: :unset,
      bytes_sent: :unset,
      request_id: :unset,
      worker: :unset,
      routing_epoch: :unset,
      operation: :unset,
      mutation_outcome: :unset
    )
      @wire_status = wire_status unless wire_status == :unset
      @bytes_sent = bytes_sent unless bytes_sent == :unset
      @request_id = request_id unless request_id == :unset
      @worker = worker unless worker == :unset
      @routing_epoch = routing_epoch unless routing_epoch == :unset
      @operation = operation unless operation == :unset
      @mutation_outcome = mutation_outcome unless mutation_outcome == :unset

      if @mutation_outcome == MutationOutcome::INDETERMINATE
        @retryability = Retryability::RECONCILE_FIRST
      elsif !@mutation_outcome.nil? && @bytes_sent.positive? && @category == Category::TRANSPORT
        @retryability = Retryability::RECONCILE_FIRST
      elsif @bytes_sent.zero? && @category == Category::TRANSPORT
        @retryability = Retryability::SAME_REQUEST
      else
        @retryability = self.class.retryability_for(
          @category,
          @bytes_sent.positive? && !@mutation_outcome.nil?,
          @mutation_outcome == MutationOutcome::INDETERMINATE
        )
      end
      self
    end

    def self.invalid_argument(message)
      new(category: Category::INVALID_ARGUMENT, message: message)
    end

    def self.unavailable(message)
      new(category: Category::UNAVAILABLE, message: message, retryability: Retryability::NEVER)
    end

    def self.transport(message)
      new(category: Category::TRANSPORT, message: message, retryability: Retryability::SAME_REQUEST)
    end

    def self.protocol(message)
      new(category: Category::PROTOCOL, message: message)
    end

    def self.not_found(message)
      new(category: Category::NOT_FOUND, message: message, retryability: Retryability::NEW_ATTEMPT)
    end

    def self.overloaded(message)
      new(category: Category::OVERLOADED, message: message, retryability: Retryability::NEVER)
    end

    def self.internal(message)
      new(category: Category::INTERNAL, message: message)
    end

    def self.from_status(status)
      err =
        case status
        when Protocol::Status::NOT_FOUND
          not_found("key was not found")
        when Protocol::Status::OVERLOADED
          overloaded("server is overloaded")
        when Protocol::Status::NOT_BOUND
          unavailable("server connection is not bound")
        when Protocol::Status::PERMISSION_DENIED
          permission_denied("server denied the request")
        when Protocol::Status::WRONG_OWNER
          protocol("server rejected Worker routing")
        when Protocol::Status::INVALID_REQUEST, Protocol::Status::UNSUPPORTED
          invalid_argument("server rejected the request")
        when Protocol::Status::INTERNAL_ERROR, Protocol::Status::OK
          internal("server reported an internal error")
        else
          protocol("server returned an unknown status")
        end
      err.enrich(wire_status: status)
      err
    end

    def self.permission_denied(message)
      new(category: Category::PERMISSION_DENIED, message: message, retryability: Retryability::NEVER)
    end
  end

  module Category
    INVALID_ARGUMENT = "invalid_argument"
    NOT_FOUND = "not_found"
    OVERLOADED = "overloaded"
    UNAVAILABLE = "unavailable"
    TRANSPORT = "transport"
    PROTOCOL = "protocol"
    INTERNAL = "internal"
    PERMISSION_DENIED = "permission_denied"
  end

  module Retryability
    NEVER = "never"
    SAME_REQUEST = "same_request"
    NEW_ATTEMPT = "new_attempt"
    RECONCILE_FIRST = "reconcile_first"
  end

  module MutationOutcome
    COMMITTED = "committed"
    REJECTED = "rejected"
    INDETERMINATE = "indeterminate"
  end

  MutationResult = Struct.new(:outcome, :error, keyword_init: true) do
    def committed?
      outcome == MutationOutcome::COMMITTED
    end

    def rejected?
      outcome == MutationOutcome::REJECTED
    end

    def indeterminate?
      outcome == MutationOutcome::INDETERMINATE
    end
  end

  module PipelineOpcode
    GET = Protocol::Opcode::GET
    PUT = Protocol::Opcode::PUT
    ERASE = Protocol::Opcode::ERASE
  end

  module PipelineOutcome
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    INDETERMINATE = "indeterminate"
  end

  PipelineRequest = Struct.new(:opcode, :key, :value, :expire_at_ns, keyword_init: true) do
    def initialize(opcode:, key:, value: "".b, expire_at_ns: 0)
      super(opcode: opcode, key: key, value: value, expire_at_ns: expire_at_ns)
    end
  end

  PipelineResponse = Struct.new(:outcome, :value, :error, keyword_init: true) do
    def succeeded?
      outcome == PipelineOutcome::SUCCEEDED
    end
  end

  # Opt-in TLS 1.3 (ADR 0020). Cleartext remains the default. When +tls+ is true the
  # client fails closed (no cleartext fallback). Hostname/SNI verification is on
  # unless +insecure_skip_verify+ is set (lab escape only).
  ClientConfig = Struct.new(
    :host, :port, :unix_socket_path, :connect_timeout, :request_timeout,
    :maximum_frame_bytes, :maximum_pipeline_requests, :maximum_pipeline_bytes,
    :tls, :tls_ca, :cert_file, :key_file, :server_name, :insecure_skip_verify,
    keyword_init: true
  ) do
    def self.defaults
      new(
        host: "127.0.0.1",
        port: 7379,
        unix_socket_path: nil,
        connect_timeout: 3.0,
        request_timeout: 5.0,
        maximum_frame_bytes: Protocol::MAX_FRAME_BYTES,
        maximum_pipeline_requests: 256,
        maximum_pipeline_bytes: 1024 * 1024,
        tls: false,
        tls_ca: nil,
        cert_file: nil,
        key_file: nil,
        server_name: nil,
        insecure_skip_verify: false
      )
    end
  end
end
