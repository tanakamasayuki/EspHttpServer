# ESP32 WebServer ライブラリ 仕様書

## 0. 概要
ESP32 の esp_http_server をベースにした軽量 Web サーバーライブラリ。  
動的ルーティング、静的ファイル配信（FS/メモリFS）、テンプレート、headInjection、gzip 自動処理などを備える。  

---

## 1. Response API

### 1.1 動的 send
```
void send(int code, const char* type,
          const uint8_t* data, size_t len);
void send(int code, const char* type,
          const String& body);

void sendText(int code, const char* type,
              const char* text);
void sendText(int code, const char* type,
              const String& text);
```

### 1.2 チャンク送信
```
void beginChunked(int code, const char* type);
void sendChunk(const uint8_t* data, size_t len);
void sendChunk(const char* text);
void sendChunk(const String& text);
void endChunked();
```

### 1.3 静的送信
```
void sendStatic();                     // serveStatic が設定した StaticInfo に従う
void sendFile(fs::FS& fs, const String& fsPath);
void sendError(int status);
```

### 1.4 リダイレクト
```
void redirect(const char* location, int status = 302);
```

- エラー／通常レスポンスに関わらずステータスコードと Location ヘッダーのみ設定し、本文は空

### 1.5 エラーレンダリング
```
using ErrorRenderer =
    std::function<void(int status,
                       Request& req,
                       Response& res)>;
void setErrorRenderer(ErrorRenderer handler);
void clearErrorRenderer();
```

- `sendStatic()` やサーバールーティングでエラーを返す場合、まず HTTP ステータスコードのみ設定し、デフォルトでは「Not Found」「Internal Server Error」など最小限のプレーンテキストを返す
- `sendError(status)` を呼び出すとステータスのみをセットし、登録済みの ErrorRenderer に処理を移譲する（未登録の場合は空ボディのまま）
- エラー画面を差し替えたい場合は `setErrorRenderer()` で描画用関数を登録し、そこで `res.sendText()` など任意のレンダリングを行う
- 未登録の場合（または `clearErrorRenderer()` 後）はデフォルトの空ボディ応答
- ErrorRenderer 内で 200 へ変更することは推奨されない（ステータスは呼び出し元が決定）

---

## 2. テンプレートエンジン

### 2.1 設定
```
using TemplateHandler =
    std::function<bool(const String& key, Print& out)>;

void setTemplateHandler(TemplateHandler cb);
void clearTemplateHandler();
```

### 2.2 挙動
- Content-Type が HTML の場合のみテンプレート処理
- `.gz` の場合はテンプレート適用不可
- `{{key}}` → HTMLエスケープして挿入
- `{{{key}}}` → 生値挿入
- ハンドラが `false` を返した場合はそのまま `{{key}}` を出す
- `send()`／`sendText()` で HTML を送る場合も、Content-Type が `text/html` かつ gzip でなければテンプレート＋headInjection が適用され、ストリーム処理でテンプレ置換が実行される
- `sendStatic()` は gzip でなければテンプレ＋headInjection、gzip ファイルはテンプレ処理無しのバイナリストリーム

---

## 3. headInjection

### 3.1 設定
```
void setHeadInjection(const char* snippet); // 非コピー
void setHeadInjection(const String& snippet);
void clearHeadInjection();
```

### 3.2 挙動
- HTML の `<head>` タグの直後に snippet を挿入
- `.gz` の場合は無効（そのまま送信するため）

---

## 4. 静的ファイルルーティング：serveStatic

### 4.1 StaticInfo
```
struct StaticInfo {
    String uri;
    String relPath;
    String fsPath;
    bool   exists;
    bool   isDir;
    bool   isGzipped;
    String logicalPath;
};
```

### 4.2 StaticHandler
```
using StaticHandler =
    std::function<void(const StaticInfo& info,
                       Request& req,
                       Response& res)>;
```

---

## 4.3 FS版 serveStatic
```
void serveStatic(const String& uriPrefix,
                 fs::FS& fs,
                 const String& basePath,
                 StaticHandler handler);
```

### FS版挙動
- `uriPrefix` を除去した relPath を解析
- basePath + relPath を参照
- `.gz` があれば優先（ただしクライアントが `.gz` を明示指定した場合に存在しなければ 404 を返す）
- relPath がディレクトリを指す場合は `index.html` → `index.htm` の順で探索し、存在すればその内容を返す（`.gz` があればそちらを優先）
- 対応するファイルが見つからなければ `StaticInfo.exists = false` となり、`sendStatic()` 側で 404 応答を返す
- 通常ファイルは `File` を開いてディレクトリかどうかを判定し、`StaticInfo.isDir` に反映
- SPA などのフォールバックは handler 内で `info.exists` を見て `res.sendFile()` / `res.sendError()` などを行う
- StaticInfo を構築して Response にセット
- handler 内で必ず 1 回 sendStatic/sendFile/redirect/sendError を呼ぶ。もし一切呼ばずにリターンした場合はライブラリ側でフォールバックし、`info.exists==true` なら自動的に `sendStatic()` を実行、`info.exists==false` なら `sendError(404)` を返す

---

## 4.4 メモリFS版 serveStatic（配列 3つ + count）
```
void serveStatic(const String& uriPrefix,
                 const char* const* paths,
                 const uint8_t* const* data,
                 const size_t* sizes,
                 size_t fileCount,
                 StaticHandler handler);
```

### メモリFS挙動
- paths[i] と relPath を照合して一致を探す
- `.gz` の優先ルールも FS と同様（明示 `.gz` 要求が解決できない場合は 404）
- relPath がディレクトリ相当（末尾 `/` または子要素が存在）なら `index.html` → `index.htm` を探索し、存在すればその内容を返す（`.gz` 優先、未検出なら 404）
- 対応するファイルが見つからなければ `StaticInfo.exists = false` となり、`sendStatic()` が 404 を返す
- `StaticInfo.fsPath` にはメモリ上の論理パスを保持し、`setStaticMemorySource()` によって実際のデータ/サイズがレスポンスへ渡される
- Response 内に backend=MemFS の静的コンテキストをセット
- handler の中で sendStatic() を呼ぶと data/size をストリーミング送信
- handler 内で何も送らずに戻った場合は FS 版と同じくフォールバックルールが適用され、`info.exists` に応じて `sendStatic()` または `sendError(404)` が自動実行される

---

## 4.5 動的ルーティング：on
```
void on(const String& uri,
        httpd_method_t method,
        RouteHandler handler);
```

- `uri` には `/user/:id` / `/files/*path` のようなパターンを指定できる（詳細は §7 パス仕様）
- `method` は `HTTP_GET` など esp_http_server の列挙値を利用
- `handler` には `void(Request& req, Response& res)` 形式のラムダ／関数を渡す
- 登録されたルートは
  - URI を正規化（クエリ除去・URL デコード・多重 `/` 解消）
  - リテラル／`:param`／`*wildcard` に分解
  - スコア（リテラル+3、パラメータ+2、ワイルドカード+1）＋登録順で最良マッチを選択
- `req.path()` で正規化済みパス、`req.pathParam("id")` でパラメータ取得
- どのルートにもマッチしない場合は 404 が返る（`onNotFound` で差し替え可能）
- 動的ハンドラがレスポンス API を一度も呼ばずに戻った場合はライブラリ側が `sendError(500)` を実行してタイムアウトを防ぐ

### 4.6 フォールバックハンドラ：onNotFound
```
void onNotFound(RouteHandler handler);
```
- `on()` / `serveStatic()` のいずれにもマッチしなかった HTTP リクエストに対して最後に呼ばれる（全体の catch-all に相当）
- SPA の index.html を返す、共通エラーページを描画する等、任意のレスポンスをここで返してよい
- 登録しない場合は従来どおり 404 を送信
- onNotFound 内でもレスポンス API を呼ばずに戻ると自動的に `sendError(404)` が実行される

---

## 5. sendStatic の共通挙動（FS / メモリFS）
1. gzip の場合  
   - `Content-Encoding: gzip`  
   - テンプレート／headInjection 無効  
   - バイト列を逐次ストリーム送信（全文を読み込まない）
2. プレーンファイル  
   - MIME 判定  
   - HTML の場合のみテンプレ＋headInjection 適用  
   - チャンクストリームや文字単位の逐次処理で送信し、全文を確保しない

---

## 6. メモリ利用ポリシー
- 可能な限り文字単位で処理を進め、全文を `String` へ読み込む実装は避ける  
- const 配列や PROGMEM から送信する場合も逐次ストリームを基本とし、最低限のバッファ以外を確保しない  
- テンプレート処理など追加の加工が必要な場合も、分割処理で省メモリなパスを検討すること  

## 7. パス仕様

### 7.1 パスパラメータ
- `/user/:id` → `id = "xxx"`
- セグメント単位でマッチ（`/` を跨がない）

### 7.2 ワイルドカード
- `/static/*path`
- 最終セグメントに残りを全て格納

### 7.3 パス正規化
- `//` → `/`
- 末尾 `/` は無視
- URL decode 後にマッチ
- クエリはマッチング前に除去

### 7.4 ルート優先順位
| セグメント種別 | スコア |
|----------------|--------|
| リテラル       | +3     |
| :param         | +2     |
| *wild          | +1     |

スコアが高いルートを優先し、同点の場合は登録順で決定する。

### 7.5 Request API
- `req.path()` で正規化済みのパスを取得
- `req.pathParam("name")` で `:name` または `*name` の値を取得
- `req.hasPathParam("name")` で存在確認

## 8. デバッグレベル方針

| レベル | 目的                   | 出力例                                       |
|--------|------------------------|----------------------------------------------|
| Error  | 致命的エラーのみ       | ハンドラ実行失敗、ファイル未存在など         |
| Info   | 通常アクセスの追跡     | 接続した IP、アクセスされた URI、HTTP メソッド|
| Debug  | 詳細な内部情報         | パラメータ解析結果、テンプレートで差し込んだ値など |

### 8.1 `[RESP]` ログフォーマット
- `Response` から出力されるログは `[RESP][任意のサブタグ...] <HTTPステータスコード> ...` の形式で必ずステータスコードを含める。
- `send()` / `sendText()` は `[RESP] 200 text/html 512 bytes` のように、コードと Content-Type/バイト数を出力する。
- `sendStatic()` は `[RESP][STATIC][FS|MEM] 200 /www/index.html (plain) origin=/wwwroot/index.html` のように、ソース区分と gzip 有無を含める。

- デフォルトは None（すべてのログを抑制）。Arduino IDE/CLI の Core Debug Level を Error/Info/Debug へ変更するとログが出力される。
- Debug レベルでは個人情報が含まれる可能性があるため、開発時のみ使用する。
- Arduino IDE/CLI の **Core Debug Level** 設定（`CORE_DEBUG_LEVEL`）でビルド時に決定され、`ESP_LOGx` の出力レベルへ反映される（デフォルトの `None` はすべてのログを抑制する）。

## 9. 使用例

### 動的
```
res.setTemplateHandler([](const String& key, Print& out){
    if (key == "name") { out.print("TANAKA"); return true; }
    return false;
});
res.setHeadInjection("<script src='/app.js'></script>");
res.sendText(200, "text/html", htmlTemplate);
```

### FS 静的
```
server.serveStatic("/view", LittleFS, "/tmpl",
  [&](const StaticInfo& info, Request& req, Response& res){
      res.setTemplateHandler(...);
      res.setHeadInjection("<script src='/app.js'></script>");
      res.sendStatic();
  });
```

### メモリFS 静的
```
server.serveStatic("/static",
                   g_paths, g_data, g_sizes, g_fileCount,
                   [&](const StaticInfo& info, Request& req, Response& res){
      if (!info.isGzipped && info.logicalPath.endsWith(".html"))
          res.setHeadInjection("<script src='/static/app.js'></script>");
      res.sendStatic();
});
```

### SPA fallback
```
if (!info.exists) {
    res.sendFile(LittleFS, "/app/index.html");
    return;
}
res.sendStatic();
```

---
