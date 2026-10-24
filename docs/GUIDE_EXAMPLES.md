# EspHttpServer — Example workflow

> 日本語版: [GUIDE_EXAMPLES.ja.md](GUIDE_EXAMPLES.ja.md)

Handle requests and build HTTP responses.

Check installation and platform requirements in the [library README](../README.md), then choose a sketch from the [example index](../examples/README.md). Version/support tables and internal capacity defaults stay in their original reference rather than being duplicated here.

## From setup to a verified result

1. Choose the smallest relevant sketch and check its board, hardware, dependencies and bundled files.
2. Match pins, peers and input files to your actual setup. Each README identifies important placeholders.
3. Open the .ino, select board/port and upload. Match Serial baud where used.
4. Locate the last completed stage: initialization, input, processing or output. Compilation does not test hardware I/O.
5. Reproduce the supplied example before changing one setting at a time.

## API and data handling

Configure the local Wi-Fi settings, wait for an IP address, then open the printed URL from a client that can reach the board. Match each route path and HTTP method. Handlers run under esp_http_server, not from Arduino loop; keep request/response references and captured local values inside the request lifetime and avoid long operations that block other requests.

Treat path, query, form, multipart and cookie values as client input. Double-brace templates escape text while triple braces insert raw output; escape untrusted text before putting it into raw HTML or script data. Cookies identify client state, not authenticated users by themselves.

Demo sessions and the login example explain mechanics; their sample credentials and plain HTTP are not a complete deployment authentication design. Finish chunked responses and check start/send results when adapting the sketch.

## When adapting it

- Distinguish bool status, processed counts and handles; do not use stale output after failure.
- Registered callbacks run later from their documented context. Decide synchronization and retained-data lifetime.
- Keep bytes, samples, frames and pixels distinct; capacity is different from valid length.
- Retain views/pointers/buffers for the required period. Copying a pointer does not copy its payload.
- Add application-specific failure handling, retry and cleanup that a minimal example may omit.

## Read the individual README

Use Prepare and run for hardware and steps, Follow the sketch for the actual APIs and precautions for feature-specific constraints. If output is missing, check initialization first, continued service calls next and input/peer readiness after that.
