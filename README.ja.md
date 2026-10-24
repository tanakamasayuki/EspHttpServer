# ESP32 HTTP Server ライブラリ

[English README](README.md)

`esp_http_server` をベースにした ESP32/Arduino 用 Web サーバースタックの仕様をまとめたリポジトリです。動的レスポンス、テンプレート/Head Injection、FS・メモリ両対応の静的配信、エラーフック、統一ログなどを一貫したルールで扱えるように整理しています。詳細仕様は [SPEC.ja.md](SPEC.ja.md) を参照してください。

初めて使う場合は[サンプル入門ガイド](docs/GUIDE_EXAMPLES.ja.md)と[各例の日英README](examples/README.ja.md)を参照してください。準備、APIの流れ、確認方法、組み込み時の注意を説明しています。

## 特長

- **レスポンス API** – `send()`/`sendText()`/`sendStatic()`/`sendFile()`、チャンク送信、リダイレクト、`sendError()` とグローバル `ErrorRenderer` を備えたレスポンス経路。
- **テンプレート & Head Injection** – `{{key}}`（HTML エスケープ）と `{{{key}}}`（生値）をストリームで差し込み、Head Injection は CSP やスクリプトといったスニペットを `<head>` 直後に挿入。どちらも gzip ペイロードでは自動的に無効化されるため、事前圧縮したアセットを安全に供給できます。
- **静的配信ライフサイクル** – `.gz` 優先や `index.html|htm` 探索、ハンドラがレスポンス忘れでも `info.exists` に応じて自動送信 or 404。
- **ルーティング & フォールバック** – `on()` はリテラル/パラメータ/ワイルドカードのスコアでマッチ、`onNotFound()` は catch-all。ハンドラ未送信時は 500/404 を自動送信してタイムアウト防止。
- **ログポリシー** – `[RESP][tag] <code> ...` 形式で統一し、Arduino の Core Debug Level（None/Error/Info/Debug）に追従。

## 仕様ダイジェスト

### Response API
```cpp
void send(int code, const char* type,
          const uint8_t* data, size_t len);
void sendText(int code, const char* type,
              const String& text);
void beginChunked(int code, const char* type);
void sendChunk(const char* text);
void sendStatic();
void sendFile(fs::FS& fs, const String& fsPath);
void sendError(int status);
void redirect(const char* location, int status = 302);
```
- HTML/非 gzip の場合はテンプレート + Head Injection が適用されます。
- `sendError()` はステータスだけをセットし、登録済み `ErrorRenderer` があればそこで共通エラーページを描画（未登録なら短いプレーンテキスト）。
- `[RESP]` ログは `sendStatic()` が `[RESP][STATIC][FS] 200 /index.html (plain) origin=/wwwroot/index.html`、エラーは `[RESP][ERR] 404` のように出力。

### 静的配信
- FS/メモリ版ともに URI を解析して `.gz` 優先、ディレクトリなら `index.html`→`index.htm` を探索。
- ハンドラが `sendStatic()`/`sendFile()`/`sendError()`/`redirect()` を呼ばずに戻った場合でも、`info.exists` に応じて自動送信（存在すれば送信、なければ 404）。
- SPA フォールバックなどは `info.exists` を見て自由に上書き可能。

### ルーティング
- `server.on()` はリテラル+3 / `:param` +2 / `*wildcard` +1 のスコアで最適ルートを選び、`req.pathParam()` で値を取得。
- `onNotFound()` は静的/動的のどちらにも当てはまらないリクエストの最終処理場所。ここでもレスポンスしなければ自動で 404。
- 動的ハンドラがレスポンス API を一度も呼ばないまま戻ると `sendError(500)` を発行。

### ログ & デバッグレベル
- Arduino IDE の **Core Debug Level**（None/Error/Info/Debug）で `ESP_LOGx` 出力を制御。`None` は `ESP_LOGE` も含めて完全抑制。
- `[RESP]` ログはフォーマットが統一されているため、テストやトレース時の grep が容易です。

## サンプル（[`examples/`](examples/)）

各スケッチは `arduino_secrets.h` があればそこから Wi-Fi 設定を読み込み、なければ
スケッチ内の `WIFI_SSID` / `WIFI_PASS` プレースホルダーを使用します。

| サンプル | アクセス先 | 主な内容 |
| --- | --- | --- |
| [BasicDynamic](examples/BasicDynamic/) | `GET /` | `sendText()` でプレーンテキストを返す最小の動的ルート。 |
| [ChunkedStream](examples/ChunkedStream/) | `GET /stream` | `beginChunked()`、`sendChunk()`、`endChunked()` で JSON をストリーム送信。 |
| [CookieSimple](examples/CookieSimple/) | `GET /` | 訪問回数 Cookie の取得・設定・削除と、その値を HTML テンプレートへ反映する例。 |
| [EmbeddedAssetsSimple](examples/EmbeddedAssetsSimple/) | `GET /embed/` | 生成済みのメモリアセットを `serveStatic()` で配信し、存在しないパスを index ページへリダイレクト。 |
| [ErrorHandling](examples/ErrorHandling/) | `GET /ok`、`GET /fail`、または存在しないパス | グローバルな `Response::setErrorRenderer()`、明示的な `sendError()`、`onNotFound()` の組み合わせ。 |
| [FormParamsSimple](examples/FormParamsSimple/) | `GET /` → `POST /submit` | `application/x-www-form-urlencoded` の値を `formParam()` で取得し、`forEachFormParam()` で列挙。 |
| [LoginPlainBind](examples/LoginPlainBind/) | `GET /` → `POST /login` | 埋め込み PlainBind UI、Head Injection した JSON、フォームによるログイン／ログアウト、AssocTree に保存するサーバー側セッションの組み合わせ。このスケッチでは `AssocTree` と `EspHelper` も使用します。 |
| [MultipartParamsSimple](examples/MultipartParamsSimple/) | `GET /` → `POST /upload` | `onMultipart()` で multipart フィールドとファイル内容をストリーム処理。 |
| [PathParams](examples/PathParams/) | `GET /users/...` または `GET /files/...` | 名前付き `:parameter`、末尾の `*wildcard`、`pathParam()` による値の取得。 |
| [QueryParamsSimple](examples/QueryParamsSimple/) | `GET /?name=esp32&lang=ja` | 名前付きクエリパラメーターの取得と全パラメーターの列挙。 |
| [SessionSimple](examples/SessionSimple/) | `GET /` | 固定 16 スロットのインメモリーストアを使ったセッション ID の作成・ローテーション・削除。 |
| [StaticFS](examples/StaticFS/) | `GET /www/` | LittleFS 上の `data/wwwroot` を `/www` で公開し、index 探索と SPA フォールバックを実行。起動前にスケッチの `data/` ディレクトリをアップロードします。 |
| [TemplateHead](examples/TemplateHead/) | `GET /template` | エスケープあり／なしのテンプレート置換と `setHeadInjection()` を同じ HTML レスポンスで使用。 |

## ツールワークフロー
- [Arduino CLI Wrapper](https://marketplace.visualstudio.com/items?itemName=tanakamasayuki.vscode-arduino-cli-wrapper) 拡張は `data/` フォルダの LittleFS/SPIFFS へのアップロードや、アセットフォルダのヘッダ変換（gzip/minify 対応）を自動化します。
- アセットを編集したら毎回再変換し、生成ヘッダと同期してください。
