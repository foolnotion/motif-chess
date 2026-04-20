## Deferred from: code review of 2-1-spdlog-logger-infrastructure (2026-04-19)

- Logger startup/config wiring is still unspecified [source/motif/import/logger.hpp:12] — this story is about infrastructure, initialization belongs in the client app (UI, cli game import client, etc)
