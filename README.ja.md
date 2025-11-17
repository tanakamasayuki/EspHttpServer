# ESP32 HTTP Server ライブラリ

[English README](README.md)

`esp_http_server` をベースにした ESP32/Arduino 用の軽量 Web サーバーライブラリ仕様です。動的レスポンス、テンプレート、head injection、gzip 対応静的ファイル配信など、Web アプリで求められる機能を統合的に提供することを目的としています。

## 特長

- **Response API** – 動的送信、チャンク送信、静的ストリーミング、リダイレクトをシンプルな関数セットで提供。`Request`/`Response` ハンドラから直感的に呼び出せるよう設計されています。
- **テンプレートエンジン** – HTML のみを対象に `{{key}}`（エスケープ）と `{{{key}}}`（生値）をサポート。ユーザー定義コールバックで値を差し込み、gzip されたアセットでは自動的に無効化します。
- **Head Injection** – HTML の `<head>` 直後へスニペットを注入し、CSP や追加スクリプトをテンプレート編集なしで挿入可能。
- **静的配信** – FS バックエンド（SPIFFS/LittleFS など）とメモリバンドル向け `serveStatic` を用意。gzip 優先、ディレクトリ/存在判定、SPA フォールバックを含む。
- **Gzip 対応** – `.gz` を検出すると `Content-Encoding: gzip` を設定し、テンプレ/Head Injection をスキップして最速で送出します。

詳細仕様は [SPEC.md](SPEC.md) を参照してください。

## コンポーネント概要

### Response ヘルパー
```cpp
void send(int code, const char* type,
          const uint8_t* data, size_t len);
void send(int code, const char* type,
          const String& body);
void beginChunked(int code, const char* type);
void sendChunk(const uint8_t* data, size_t len);
void sendStatic();
void sendFile(fs::FS& fs, const String& fsPath);
void redirect(const char* location, int status = 302);
```

これらの API は `esp_http_server` をラップし、テキスト/バイナリ送信、チャンク転送、静的ファイル配信を簡単にします。`sendStatic()` は `serveStatic` がセットした `StaticInfo` を利用します。

### テンプレート & Head Injection

`setTemplateHandler` に `std::function<bool(const String&, Print&)>` を設定することで、プレースホルダーごとに任意の文字列を出力可能です。head injection は `const char*` もしくは `String` を保持し、非 gzip の HTML 応答にのみ適用されます。

### 静的ルーティング

2 種類の `serveStatic` が `StaticInfo` を構築し、ユーザーハンドラを呼び出します。

- **FS バックエンド** – URI プレフィックスから `basePath` を解析し、`.gz` の存在チェックや `exists`/`isDir`/`logicalPath` をセット。
- **メモリバックエンド** – 渡された配列群からパスを探索し、FS 版と同じ gzip 優先ルールでデータを提供。

ハンドラ内では `sendStatic()`、`sendFile()`、`redirect()` のいずれかを必ず 1 回呼び出してレスポンスを完了させます。

### 使用例

```cpp
server.serveStatic("/view", LittleFS, "/tmpl",
  [&](const StaticInfo& info, Request& req, Response& res){
      res.setTemplateHandler(...);
      res.setHeadInjection("<script src='/app.js'></script>");
      res.sendStatic();
  });
```

```cpp
server.serveStatic("/static", g_paths, g_data, g_sizes, g_fileCount,
  [&](const StaticInfo& info, Request& req, Response& res){
      if (!info.isGzipped && info.logicalPath.endsWith(".html")) {
          res.setHeadInjection("<script src='/static/app.js'></script>");
      }
      res.sendStatic();
  });
```

SPA 向けには `info.exists` を検査し、存在しない場合に `sendFile()` で `/app/index.html` などを返すことでフォールバックできます。

### デバッグレベル

Arduino IDE の **Core Debug Level**（または Arduino CLI の `--build-property build.code.debug=<level>`）設定に連動して `ESP_LOGx` の出力が決まります。デフォルトの `None` では `ESP_LOGE` さえ抑制されるため、開発時は少なくとも `Error` 以上に設定してください。

- `None`（デフォルト）: すべてのログを非表示
- `Error`: `ESP_LOGE` による致命的エラーのみ
- `Info`: リクエストラインや静的ルートの解決を表示
- `Debug`: パスパラメータや静的ファイル解決の詳細も出力

## サンプルスケッチ

- **BasicDynamic** – 動的テキストレスポンスの最小例。
- **ChunkedStream** – チャンク送信で JSON をストリーミング。
- **TemplateHead** – テンプレートと head injection の併用例。
- **StaticFS** – `examples/StaticFS/data` の内容を LittleFS へ転送して配信。
- **EmbeddedAssetsSimple** – `examples/EmbeddedAssetsSimple/assets_www` を Arduino CLI Wrapper でヘッダー化し、`/embed` からフラッシュ上のファイルを配信。
- **PathParams** – `req.pathParam()` で `:id` や `*path` を扱うルーティングのサンプル。

## VS Code ワークフロー

[Arduino CLI Wrapper](https://marketplace.visualstudio.com/items?itemName=tanakamasayuki.vscode-arduino-cli-wrapper) 拡張機能を使うと以下が自動化できます。

- `data/` フォルダ（例: `examples/StaticFS/data`）を LittleFS / SPIFFS へアップロード。
- `examples/assetsTest/assets_www` のようなアセットを `.assetsconfig` の設定に従ってヘッダーファイル（`assets_www_embed.h` など）へ変換し、必要に応じて minify / gzip 処理を実施。

アセットを編集した際は拡張機能を再実行し、生成ヘッダーとの同期を保ってください。

## 今後のタスク例

1. `Request`/`Response` クラスを実装し、`esp_http_server` とバインド。
2. LittleFS/SPIFFS/SD などの FS アダプタとメモリバンドル生成ツールを整備。
3. gzip 生成と MIME 判定ユーティリティを追加し、ビルド時に資産を前処理。
