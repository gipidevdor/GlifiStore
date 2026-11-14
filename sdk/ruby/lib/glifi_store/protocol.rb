# frozen_string_literal: true

module GlifiStore
  # Canonical wire-protocol v2 codec and Worker routing.
  module Protocol
    VERSION = 2
    REQUEST_HEADER_BYTES = 40
    RESPONSE_HEADER_BYTES = 40
    MAX_FRAME_BYTES = 2 * 1024 * 1024
    NO_WORKER = 0xFFFF_FFFF
    IDENTITY = "GlifiStore/2".b
    ROUTING_ALG_FNV1A64_V1 = 1
    ROUTING_ALG_SIPHASH24_V1 = 2
    WORKER_ROUTING_SIP_KEY1_XOR = 0x6a09e667f3bcc909
    INIT_IDENTITY_EXTENDED_BYTES = IDENTITY.bytesize + 1 + 4 + 8

    WorkerRouting = Struct.new(:algorithm, :seed, keyword_init: true) do
      def initialize(algorithm: ROUTING_ALG_FNV1A64_V1, seed: 0)
        super(algorithm: algorithm, seed: seed)
      end

      def keyed?
        algorithm == ROUTING_ALG_SIPHASH24_V1
      end
    end

    module Opcode
      INIT = 1
      PING = 2
      GET = 3
      PUT = 4
      ERASE = 5
      BIND_WORKER = 6
      HEALTH = 7
      READY = 8
      STATS = 9
      BACKUP = 10
    end

    module Status
      OK = 0
      INVALID_REQUEST = 1
      UNSUPPORTED = 2
      INTERNAL_ERROR = 3
      NOT_FOUND = 4
      OVERLOADED = 5
      WRONG_OWNER = 6
      NOT_BOUND = 7
      PERMISSION_DENIED = 8
    end

    Request = Struct.new(
      :opcode, :request_id, :expire_at_ns, :target_worker, :key, :value,
      keyword_init: true
    )
    Response = Struct.new(
      :status, :request_id, :owner_worker, :worker_count, :routing_epoch, :value,
      keyword_init: true
    )

    REQUEST_HEADER = "L<S<CCQ<L<L<Q<L<L<"
    RESPONSE_HEADER = "L<S<S<Q<L<L<L<L<Q<"

    module_function

    def request_frame_size(key, value)
      REQUEST_HEADER_BYTES + key.bytesize + value.bytesize
    end

    def encode_request(opcode, request_id, key: "".b, value: "".b, expire_at_ns: 0, target_worker: NO_WORKER)
      key = binary!(key)
      value = binary!(value)
      validate_request_fields!(opcode, key, value, expire_at_ns, target_worker)
      u64!(request_id, "request_id")
      u64!(expire_at_ns, "expire_at_ns")
      u32!(target_worker, "target_worker")
      frame_size = request_frame_size(key, value)
      raise ArgumentError, "request exceeds the protocol frame limit" if frame_size > MAX_FRAME_BYTES

      [
        frame_size, VERSION, opcode, 0, request_id,
        key.bytesize, value.bytesize, expire_at_ns, target_worker, 0
      ].pack(REQUEST_HEADER) + key + value
    end

    def decode_request(frame, maximum_frame_bytes = MAX_FRAME_BYTES)
      frame = binary!(frame)
      raise ArgumentError, "request is shorter than its header" if frame.bytesize < REQUEST_HEADER_BYTES

      frame_size, version, opcode, flags, request_id, key_size, value_size,
        expire_at_ns, target_worker, reserved = frame.unpack(REQUEST_HEADER)
      if frame_size != frame.bytesize || frame_size > maximum_frame_bytes
        raise ArgumentError, "request frame extent is invalid"
      end
      raise ArgumentError, "request protocol version is unsupported" if version != VERSION
      raise ArgumentError, "request canonical fields are invalid" if flags != 0 || reserved != 0
      if REQUEST_HEADER_BYTES + key_size + value_size != frame_size
        raise ArgumentError, "request payload extent is invalid"
      end
      raise ArgumentError, "request opcode is unknown" unless valid_opcode?(opcode)

      key_start = REQUEST_HEADER_BYTES
      value_start = key_start + key_size
      key = frame.byteslice(key_start, key_size).b
      value = frame.byteslice(value_start, value_size).b
      validate_request_fields!(opcode, key, value, expire_at_ns, target_worker)
      Request.new(
        opcode: opcode,
        request_id: request_id,
        expire_at_ns: expire_at_ns,
        target_worker: target_worker,
        key: key,
        value: value
      )
    end

    def encode_response(status, request_id, value: "".b, owner_worker: NO_WORKER,
                        worker_count: 0, routing_epoch: 0)
      raise ArgumentError, "status is not defined by wire protocol v2" unless valid_status?(status)

      value = binary!(value)
      u64!(request_id, "request_id")
      u32!(owner_worker, "owner_worker")
      u32!(worker_count, "worker_count")
      u64!(routing_epoch, "routing_epoch")
      frame_size = RESPONSE_HEADER_BYTES + value.bytesize
      raise ArgumentError, "response exceeds the protocol frame limit" if frame_size > MAX_FRAME_BYTES

      [
        frame_size, VERSION, status, request_id, value.bytesize,
        owner_worker, worker_count, 0, routing_epoch
      ].pack(RESPONSE_HEADER) + value
    end

    def decode_response(frame, maximum_frame_bytes = MAX_FRAME_BYTES)
      frame = binary!(frame)
      raise ArgumentError, "response is shorter than its header" if frame.bytesize < RESPONSE_HEADER_BYTES

      frame_size, version, status, request_id, value_size, owner_worker,
        worker_count, reserved, routing_epoch = frame.unpack(RESPONSE_HEADER)
      if frame_size != frame.bytesize || frame_size > maximum_frame_bytes
        raise ArgumentError, "response frame extent is invalid"
      end
      raise ArgumentError, "response protocol version is unsupported" if version != VERSION
      raise ArgumentError, "response reserved field is noncanonical" if reserved != 0
      if RESPONSE_HEADER_BYTES + value_size != frame_size
        raise ArgumentError, "response value extent is invalid"
      end
      raise ArgumentError, "response status is unknown" unless valid_status?(status)

      Response.new(
        status: status,
        request_id: request_id,
        owner_worker: owner_worker,
        worker_count: worker_count,
        routing_epoch: routing_epoch,
        value: frame.byteslice(RESPONSE_HEADER_BYTES, value_size).to_s.b
      )
    end

    def fnv1a64(key)
      key = binary!(key)
      value = 14_695_981_039_346_656_037
      key.each_byte do |byte|
        value ^= byte
        value = (value * 1_099_511_628_211) & 0xFFFF_FFFF_FFFF_FFFF
      end
      value
    end

    # SipHash-2-4 (Aumasson/Bernstein). Independent implementation for siphash24-v1 routing;
    # see repository THIRD_PARTY_NOTICES.md. Not a copy of a third-party source tree.
    def siphash24(key, k0, k1)
      key = binary!(key)
      k0 &= 0xFFFF_FFFF_FFFF_FFFF
      k1 &= 0xFFFF_FFFF_FFFF_FFFF
      rotl = ->(value, shift) { ((value << shift) | (value >> (64 - shift))) & 0xFFFF_FFFF_FFFF_FFFF }
      sipround = lambda do |v0, v1, v2, v3|
        v0 = (v0 + v1) & 0xFFFF_FFFF_FFFF_FFFF
        v1 = rotl.call(v1, 13)
        v1 ^= v0
        v0 = rotl.call(v0, 32)
        v2 = (v2 + v3) & 0xFFFF_FFFF_FFFF_FFFF
        v3 = rotl.call(v3, 16)
        v3 ^= v2
        v0 = (v0 + v3) & 0xFFFF_FFFF_FFFF_FFFF
        v3 = rotl.call(v3, 21)
        v3 ^= v0
        v2 = (v2 + v1) & 0xFFFF_FFFF_FFFF_FFFF
        v1 = rotl.call(v1, 17)
        v1 ^= v2
        v2 = rotl.call(v2, 32)
        [v0, v1, v2, v3]
      end
      v0 = k0 ^ 0x736f6d6570736575
      v1 = k1 ^ 0x646f72616e646f6d
      v2 = k0 ^ 0x6c7967656e657261
      v3 = k1 ^ 0x7465646279746573
      length = key.bytesize
      offset = 0
      while offset + 8 <= length
        message = key.byteslice(offset, 8).unpack1("Q<")
        v3 ^= message
        v0, v1, v2, v3 = sipround.call(v0, v1, v2, v3)
        v0, v1, v2, v3 = sipround.call(v0, v1, v2, v3)
        v0 ^= message
        offset += 8
      end
      message = length << 56
      key.byteslice(offset, length - offset).to_s.each_byte.with_index do |byte, index|
        message |= byte << (8 * index)
      end
      v3 ^= message
      v0, v1, v2, v3 = sipround.call(v0, v1, v2, v3)
      v0, v1, v2, v3 = sipround.call(v0, v1, v2, v3)
      v0 ^= message
      v2 ^= 0xff
      4.times { v0, v1, v2, v3 = sipround.call(v0, v1, v2, v3) }
      v0 ^ v1 ^ v2 ^ v3
    end

    def validate_worker_routing!(routing)
      case routing.algorithm
      when ROUTING_ALG_FNV1A64_V1
        raise ArgumentError, "fnv1a64-v1 Worker routing requires a zero hash seed" unless routing.seed.zero?
      when ROUTING_ALG_SIPHASH24_V1
        nil
      else
        raise ArgumentError, "unsupported Worker routing algorithm"
      end
    end

    def hash_key_routing(key, routing = WorkerRouting.new)
      validate_worker_routing!(routing)
      if routing.algorithm == ROUTING_ALG_SIPHASH24_V1
        siphash24(key, routing.seed, routing.seed ^ WORKER_ROUTING_SIP_KEY1_XOR)
      else
        fnv1a64(key)
      end
    end

    def encode_init_identity(routing = WorkerRouting.new)
      validate_worker_routing!(routing)
      return IDENTITY.b unless routing.keyed?

      IDENTITY.b + "\x00".b + [routing.algorithm, routing.seed].pack("L<Q<")
    end

    def decode_init_identity(value)
      value = binary!(value)
      if value == IDENTITY.b
        return WorkerRouting.new
      end
      if value.bytesize != INIT_IDENTITY_EXTENDED_BYTES
        raise ArgumentError, "server INIT identity value has unexpected length"
      end
      if value.byteslice(0, IDENTITY.bytesize) != IDENTITY.b || value.getbyte(IDENTITY.bytesize) != 0
        raise ArgumentError, "server INIT identity prefix is invalid"
      end
      algorithm, seed = value.byteslice(IDENTITY.bytesize + 1, 12).unpack("L<Q<")
      state = WorkerRouting.new(algorithm: algorithm, seed: seed)
      validate_worker_routing!(state)
      raise ArgumentError, "server INIT extended identity must use siphash24-v1 routing" unless state.keyed?

      state
    end

    def worker_for(key, worker_count, routing = WorkerRouting.new)
      raise ArgumentError, "worker_count must be positive" if worker_count <= 0

      hash_key_routing(key, routing) % worker_count
    end

    def valid_opcode?(opcode)
      opcode.between?(Opcode::INIT, Opcode::BACKUP)
    end

    def valid_status?(status)
      status.between?(Status::OK, Status::PERMISSION_DENIED)
    end

    def binary!(payload)
      case payload
      when String
        payload.b
      when nil
        "".b
      else
        raise TypeError, "key and value must be strings"
      end
    end

    def u64!(value, field)
      raise ArgumentError, "#{field} is outside unsigned 64-bit range" unless value.is_a?(Integer) && value.between?(0, 0xFFFF_FFFF_FFFF_FFFF)

      value
    end

    def u32!(value, field)
      raise ArgumentError, "#{field} is outside unsigned 32-bit range" unless value.is_a?(Integer) && value.between?(0, 0xFFFF_FFFF)

      value
    end

    def validate_request_fields!(opcode, key, value, expire_at_ns, target_worker)
      case opcode
      when Opcode::INIT
        if !key.empty? || !value.empty? || expire_at_ns != 0 || target_worker != NO_WORKER
          raise ArgumentError, "INIT request cannot carry key, value, expiry, or target_worker"
        end
      when Opcode::PING
        if !key.empty? || expire_at_ns != 0 || target_worker != NO_WORKER
          raise ArgumentError, "PING request cannot carry key, expiry, or target_worker"
        end
      when Opcode::GET
        if key.empty? || !value.empty? || expire_at_ns != 0 || target_worker != NO_WORKER
          raise ArgumentError, "GET request requires a key and cannot carry value, expiry, or target_worker"
        end
      when Opcode::PUT
        if key.empty? || target_worker != NO_WORKER
          raise ArgumentError, "PUT request requires a key and cannot carry target_worker"
        end
      when Opcode::ERASE
        if key.empty? || !value.empty? || expire_at_ns != 0 || target_worker != NO_WORKER
          raise ArgumentError, "ERASE request requires a key and cannot carry value, expiry, or target_worker"
        end
      when Opcode::BIND_WORKER
        if !key.empty? || !value.empty? || expire_at_ns != 0
          raise ArgumentError, "BIND_WORKER request cannot carry key, value, or expiry"
        end
        raise ArgumentError, "BIND_WORKER request requires an explicit target_worker" if target_worker == NO_WORKER
      when Opcode::HEALTH, Opcode::READY, Opcode::STATS
        if !key.empty? || !value.empty? || expire_at_ns != 0 || target_worker != NO_WORKER
          raise ArgumentError, "lifecycle probe cannot carry key, value, expiry, or target_worker"
        end
      when Opcode::BACKUP
        if key.empty? || !value.empty? || expire_at_ns != 0 || target_worker != NO_WORKER
          raise ArgumentError,
                "BACKUP requires a destination path key and no value, expiry, or target_worker"
        end
      else
        raise ArgumentError, "opcode is not defined by wire protocol v2"
      end
    end
    private_class_method :binary!, :u64!, :u32!, :validate_request_fields!
  end
end
