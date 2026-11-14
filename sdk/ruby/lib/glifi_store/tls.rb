# frozen_string_literal: true

require "openssl"
require "timeout"

module GlifiStore
  # Opt-in TLS 1.3 helpers (ADR 0020). Cleartext remains the default.
  # Fail closed: no cleartext fallback when TLS is requested.
  module Tls
    module_function

    def tls13_available?
      defined?(OpenSSL::SSL::TLS1_3_VERSION)
    end

    def build_ssl_context(config)
      unless tls13_available?
        raise Error.unavailable(
          "TLS 1.3 was requested but this Ruby/OpenSSL build does not expose TLS 1.3 (fail closed)"
        )
      end

      context = OpenSSL::SSL::SSLContext.new
      # Ruby/OpenSSL builds vary: some expose TLS1_3_VERSION but not
      # min_version=/max_version= (older openssl gem on hosted Ubuntu).
      if context.respond_to?(:min_version=) && context.respond_to?(:max_version=)
        context.min_version = OpenSSL::SSL::TLS1_3_VERSION
        context.max_version = OpenSSL::SSL::TLS1_3_VERSION
      elsif context.respond_to?(:ssl_version=)
        context.ssl_version = :TLSv1_3
      else
        raise Error.unavailable(
          "TLS 1.3 was requested but this Ruby/OpenSSL build cannot pin TLS 1.3 (fail closed)"
        )
      end
      if config.insecure_skip_verify
        context.verify_mode = OpenSSL::SSL::VERIFY_NONE
      else
        context.verify_mode = OpenSSL::SSL::VERIFY_PEER
        context.verify_hostname = true
        if config.tls_ca && !config.tls_ca.empty?
          context.ca_file = config.tls_ca
        else
          context.set_default_paths
        end
      end
      if config.cert_file && !config.cert_file.empty? && config.key_file && !config.key_file.empty?
        context.cert = OpenSSL::X509::Certificate.new(File.binread(config.cert_file))
        context.key = OpenSSL::PKey.read(File.binread(config.key_file))
      end
      context
    rescue Errno::ENOENT, OpenSSL::OpenSSLError => e
      raise Error.invalid_argument("cannot load TLS material: #{e.message}")
    end

    def wrap_socket(tcp_socket, config)
      context = build_ssl_context(config)
      server_name = if config.server_name && !config.server_name.empty?
                      config.server_name
                    else
                      config.host
                    end
      ssl = OpenSSL::SSL::SSLSocket.new(tcp_socket, context)
      ssl.sync_close = true
      ssl.hostname = server_name if server_name && !server_name.empty?
      begin
        Timeout.timeout(config.connect_timeout) { ssl.connect }
      rescue Timeout::Error
        begin
          ssl.close
        rescue StandardError
          nil
        end
        raise Error.unavailable("TLS handshake timed out")
      rescue OpenSSL::SSL::SSLError => e
        begin
          ssl.close
        rescue StandardError
          nil
        end
        raise Error.unavailable("TLS handshake failed: #{sanitize_error(e)}")
      end
      ssl
    end

    def sanitize_error(error)
      msg = error.message.to_s
      return "certificate verification failed" if msg.match?(/certificate verify failed|hostname mismatch/i)
      return "TLS failure" if msg.bytesize > 200

      msg
    end
  end
end
