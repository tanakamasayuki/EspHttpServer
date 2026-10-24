# FormParamsSimple

> 日本語版: [README.ja.md](README.ja.md)

Read URL-encoded form fields from a POST request.

## Prepare and run

1. Check the [example guide](../../docs/GUIDE_EXAMPLES.md) for setup and hardware. Use the root README for supported boards and dependencies.
2. Open [FormParamsSimple.ino](FormParamsSimple.ino) and select the actual board/port. Prepare the dependencies used by these includes:

   `<WiFi.h>`, `<EspHttpServer.h>`

3. Upload and open the Serial monitor at 115200 baud. Check startup messages and input conditions in the source.

## Follow the sketch

These APIs are listed in their first source order. Registering a callback does not execute its body at that point.

1. `res.sendText(...)` — Choose status and content type, then finish the request with the intended text response.
2. `req.formParam(...)` — Read a submitted form field rather than a URL path or query value.
3. `res.setTemplateHandler(...)` — Provide values during response rendering; retain captures until rendering finishes.
4. `server.on(...)` — Register the route path and HTTP method; the handler receives request-scoped objects.
5. `server.begin(...)` — Initialize this component after its prerequisites; inspect the returned status where provided.

Routes in this sketch: `GET /`, `POST /submit`

## API usage and precautions

Configure the local Wi-Fi settings, wait for an IP address, then open the printed URL from a client that can reach the board. Match each route path and HTTP method. Handlers run under esp_http_server, not from Arduino loop; keep request/response references and captured local values inside the request lifetime and avoid long operations that block other requests.

Treat path, query, form, multipart and cookie values as client input. Double-brace templates escape text while triple braces insert raw output; escape untrusted text before putting it into raw HTML or script data. Cookies identify client state, not authenticated users by themselves.

Demo sessions and the login example explain mechanics; their sample credentials and plain HTTP are not a complete deployment authentication design. Finish chunked responses and check start/send results when adapting the sketch.

## Inputs and observable output

These are selected output formats from the source. printf specifiers are replaced by runtime values; inspect the surrounding conditions rather than assuming these lines always occur in this order.

```text
Server ready: http://%s/
```

## Verify and troubleshoot

Read URL-encoded form fields from a POST request and verify the result on its output. First use the supplied input, then change one configuration or data source at a time.

| Symptom | First check |
|---|---|
| No output | Board/dependencies, initialization results, output destination and Serial baud |
| Starts but stops making progress | Wait conditions, buffer space, peer/input readiness and service calls |
| Unexpected values | Lengths/units, format, data lifetime, shared state and partial results |

See the [guide](../../docs/GUIDE_EXAMPLES.md) for shared principles and the [example index](../README.md) for the next sketch.
