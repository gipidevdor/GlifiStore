#!/usr/bin/env ruby
# frozen_string_literal: true

require "optparse"
require "pathname"
require_relative "../lib/glifi_store"

options = {
  host: "127.0.0.1",
  workers: 1,
  ops: 100_000,
  pipeline: 128,
  warmup: 1,
  repeats: 7,
  concurrent: nil
}
parser = OptionParser.new do |opts|
  opts.banner = "Usage: client_benchmark.rb --port N [options]"
  opts.on("--host HOST", String) { |v| options[:host] = v }
  opts.on("--port PORT", Integer) { |v| options[:port] = v }
  opts.on("--workers N", Integer) { |v| options[:workers] = v }
  opts.on("--ops N", Integer) { |v| options[:ops] = v }
  opts.on("--pipeline N", Integer) { |v| options[:pipeline] = v }
  opts.on("--warmup N", Integer) { |v| options[:warmup] = v }
  opts.on("--repeats N", Integer) { |v| options[:repeats] = v }
  opts.on("--[no-]concurrent") { |v| options[:concurrent] = v }
end
parser.parse!
abort("--port is required") if options[:port].nil?
abort("numeric arguments are outside benchmark limits") if options[:workers] < 1 || options[:ops] < 1 ||
  options[:pipeline] < 1 || options[:warmup].negative? || options[:repeats] < 1

use_concurrent = if options[:concurrent].nil?
                   options[:workers] > 1
                 else
                   options[:concurrent]
                 end

requests = Array.new(options[:workers]) { [] }
remaining = options[:workers].times.map do |i|
  (options[:ops] / options[:workers]) + (i < (options[:ops] % options[:workers]) ? 1 : 0)
end
candidate = 0
while remaining.any?(&:positive?)
  key = format("ruby-bench-%012d", candidate).b
  owner = GlifiStore::Protocol.worker_for(key, options[:workers])
  if remaining[owner].positive?
    value = ([candidate & 0xFF].pack("C") * 64)
    requests[owner] << GlifiStore::PipelineRequest.new(
      opcode: GlifiStore::PipelineOpcode::PUT, key: key, value: value
    )
    requests[owner] << GlifiStore::PipelineRequest.new(
      opcode: GlifiStore::PipelineOpcode::GET, key: key
    )
    remaining[owner] -= 1
  end
  candidate += 1
end

batch_frames = options[:pipeline] * 2
batches = []
max_rounds = 0
requests.each do |worker_requests|
  worker_batches = []
  offset = 0
  while offset < worker_requests.length
    last = [offset + batch_frames - 1, worker_requests.length - 1].min
    worker_batches << worker_requests[offset..last]
    offset = last + 1
  end
  max_rounds = worker_batches.length if worker_batches.length > max_rounds
  batches << worker_batches
end

config = GlifiStore::ClientConfig.defaults
config.host = options[:host]
config.port = options[:port]
config.maximum_pipeline_requests = batch_frames
client = GlifiStore::Client.connect(config)
abort("server Worker count does not match --workers") if client.worker_count != options[:workers]

def validate_batch!(batch, responses)
  raise "pipeline response count mismatch" if responses.length != batch.length

  batch.each_with_index do |request, index|
    raise "pipeline request failed" unless responses[index].succeeded?

    next unless request.opcode == GlifiStore::PipelineOpcode::GET
    raise "pipeline GET value mismatch" if responses[index].value != batch[index - 1].value
  end
end

run_once =
  if use_concurrent
    lambda do
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      max_rounds.times do |round|
        wave = batches.map { |worker_batches| round < worker_batches.length ? worker_batches[round] : [] }
        # Concurrent: one pipeline per Worker via execute_batch flattened... use threads like batch.
        threads = []
        results = Array.new(options[:workers])
        options[:workers].times do |worker|
          next if wave[worker].empty?

          threads << Thread.new(worker) do |w|
            results[w] = client.execute_pipeline(wave[w])
          end
        end
        threads.each(&:join)
        options[:workers].times do |worker|
          next if wave[worker].empty?

          validate_batch!(wave[worker], results[worker])
        end
      end
      Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
    end
  else
    lambda do
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      batches.each do |worker_batches|
        worker_batches.each do |batch|
          validate_batch!(batch, client.execute_pipeline(batch))
        end
      end
      Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
    end
  end

options[:warmup].times { run_once.call }
samples = options[:repeats].times.map { run_once.call }
client.close

operation_count = options[:ops] * 2
rates = samples.map { |s| operation_count / s }
sorted = samples.sort
median_s = sorted.length.odd? ? sorted[sorted.length / 2] : (sorted[sorted.length / 2 - 1] + sorted[sorted.length / 2]) / 2.0
sorted_rates = rates.sort
median_r = sorted_rates.length.odd? ? sorted_rates[sorted_rates.length / 2] : (sorted_rates[sorted_rates.length / 2 - 1] + sorted_rates[sorted_rates.length / 2]) / 2.0
execution = use_concurrent ? "single-process-worker-concurrent" : "single-process-worker-sequential"

puts "# glifistore Ruby client benchmark"
puts "# sdk_version=#{GlifiStore::VERSION} runtime=sync execution=#{execution} " \
     "workers=#{options[:workers]} pipeline_pairs=#{options[:pipeline]} operations=#{operation_count}"
printf(
  "name=ruby_client_pipeline_read_after_write sdk_version=%s runtime=sync execution=%s " \
  "workers=%d pipeline_pairs=%d operations=%d samples=%d median_seconds=%.9f min_seconds=%.9f " \
  "max_seconds=%.9f median_ops_per_second=%.3f min_ops_per_second=%.3f max_ops_per_second=%.3f\n",
  GlifiStore::VERSION, execution, options[:workers], options[:pipeline], operation_count,
  samples.length, median_s, sorted.first, sorted.last, median_r, sorted_rates.first, sorted_rates.last
)
