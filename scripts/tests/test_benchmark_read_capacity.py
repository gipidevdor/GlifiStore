import asyncio
from pathlib import Path
from types import SimpleNamespace
import unittest

from scripts import benchmark_read_capacity as benchmark
from glifistore.protocol import Opcode, Status, decode_request, encode_response


class OfferedCapacityTests(unittest.IsolatedAsyncioTestCase):
    async def test_slow_server_does_not_turn_offered_load_into_closed_loop(self):
        async def server(reader, writer):
            try:
                while True:
                    header = await reader.readexactly(40)
                    size = int.from_bytes(header[:4], "little")
                    request = decode_request(header + await reader.readexactly(size - 40))
                    if request.opcode == Opcode.GET:
                        await asyncio.sleep(0.005)
                    value = b"v" if request.opcode == Opcode.GET else b""
                    writer.write(encode_response(Status.OK, request.request_id, value=value,
                                                 owner_worker=0, worker_count=1, routing_epoch=1))
                    await writer.drain()
            except (asyncio.IncompleteReadError, ConnectionError):
                pass
            finally:
                writer.close()
                await writer.wait_closed()

        listener = await asyncio.start_server(server, "127.0.0.1", 0)
        async with listener:
            result = await benchmark.workload(SimpleNamespace(clients=1, value_size=1,
                rate=5000, duration_ms=500, slo_us=5000), listener.sockets[0].getsockname()[1])
        self.assertEqual(result["scheduled"], 2500)
        self.assertEqual(result["completed"], result["admitted"])
        self.assertEqual(result["scheduled"], result["admitted"] + result["missed"])
        self.assertGreater(result["missed"], 1000)
        self.assertFalse(result["all_offers_served_and_p99_within_slo"])
        self.assertLessEqual(max(window["outstanding"] for window in result["windows"]), 64)

    def test_experiment_rejects_excess_work(self):
        for args in (("--rate", "250001"), ("--helpers", "5"),
                     ("--rate", "250000", "--duration-ms", "30000")):
            with self.subTest(args=args), self.assertRaises(SystemExit):
                benchmark.arguments(["--binary", str(Path(__file__).resolve()),
                                     "--output", "/unused-output", *args])


if __name__ == "__main__":
    unittest.main()
