// ============================================================
//  main.cpp — Gmai チャットアプリ（WebView2 + Gemini API）
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <oleauto.h>

// WebView2 SDK ヘッダー
#include "webview2/WebView2.h"

#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <thread>

#pragma comment(lib, "wininet.lib")

// ============================================================
//  カスタムウィンドウメッセージ（ワーカースレッド → UIスレッドへの通知用）
// ============================================================
#define WM_GEMINI_REPLY (WM_APP + 1)

// 通信データ受渡用構造体
struct ThreadData {
    std::string userMessage;
    std::string requestJson;
};

// ============================================================
//  グローバル変数
// ============================================================
static HWND                     g_hwnd       = nullptr;
static ICoreWebView2*           g_webview    = nullptr;
static ICoreWebView2Controller* g_controller = nullptr;

// ------------------------------------------------------------
// ★【APIキー変数】★★★
// Google_API_Key の中身は実行時に .env から読み込まれます。
// コード内にキー文字列を直接代入・ハードコードしません。
// ------------------------------------------------------------
static std::string Google_API_Key;

static std::string g_systemInstruction;
static std::string g_familyContext;
static std::string g_exePath;   // exe のあるディレクトリ（Shift_JIS）
static std::string g_historyPath;

// ============================================================
//  ユーティリティ: exe のディレクトリパスを取得
// ============================================================
std::string getExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    return path.substr(0, path.find_last_of("\\/") + 1);
}

std::wstring getExeDirW() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    return path.substr(0, path.find_last_of(L"\\/") + 1);
}

// ============================================================
//  ユーティリティ: std::string (UTF-8) → std::wstring
// ============================================================
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], wlen);
    return ws;
}

// ============================================================
//  ユーティリティ: std::wstring → std::string (UTF-8)
// ============================================================
std::string wideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

// ============================================================
//  1️⃣ .env ファイルの読み込み（実行時に外部ファイルから取得）
// ============================================================
void loadEnv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        MessageBoxA(nullptr,
            ".env ファイルが見つかりません。\n"
            "exe と同じフォルダに .env を配置してください。",
            "エラー", MB_OK | MB_ICONERROR);
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key   = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        // クォートの除去
        if (value.size() >= 2 &&
            ((value.front() == '"'  && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        // \n エスケープシーケンスの展開
        std::string expanded;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == 'n') {
                expanded += '\n';
                ++i;
            } else {
                expanded += value[i];
            }
        }
        value = expanded;

        if (key == "Google_API_Key" || key == "GOOGLE_API_KEY" || key == "GEMINI_API_KEY") {
            Google_API_Key = value;
        }
        else if (key == "SYSTEM_INSTRUCTION") {
            g_systemInstruction = value;
        }
        else if (key == "FAMILY_CONTEXT") {
            g_familyContext = value;
        }
    }
}

// ============================================================
//  2️⃣ history.json の自動リセット
// ============================================================
void resetHistory(const std::string& path) {
    DeleteFileA(path.c_str());
    std::ofstream file(path);
    if (file.is_open()) {
        file << "[]";
    }
}

std::string readHistory(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "[]";
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    return content.empty() ? "[]" : content;
}

void writeHistory(const std::string& path, const std::string& json) {
    std::ofstream file(path, std::ios::trunc);
    file << json;
}

// JSON文字列エスケープ
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 32);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Gemini API リクエストJSON構築
std::string buildRequestJson(const std::string& userMessage, const std::string& historyPath) {
    std::string history = readHistory(historyPath);
    std::ostringstream json;
    json << "{\n";

    if (!g_systemInstruction.empty()) {
        json << "  \"system_instruction\": {\n";
        json << "    \"parts\": [{\"text\": \"" << jsonEscape(g_systemInstruction) << "\"}]\n";
        json << "  },\n";
    }

    json << "  \"contents\": [\n";

    bool hasItem = false;
    if (!g_familyContext.empty()) {
        json << "    {\"role\": \"user\",  \"parts\": [{\"text\": \"" << jsonEscape(g_familyContext) << "\"}]},\n";
        json << "    {\"role\": \"model\", \"parts\": [{\"text\": \"わかりました。\"}]}";
        hasItem = true;
    }

    if (history.size() > 2) {
        std::string inner = history.substr(1, history.size() - 2);
        while (!inner.empty() && std::isspace((unsigned char)inner.front())) inner.erase(inner.begin());
        while (!inner.empty() && std::isspace((unsigned char)inner.back())) inner.pop_back();
        if (!inner.empty()) {
            if (hasItem) json << ",\n";
            json << "    " << inner;
            hasItem = true;
        }
    }

    if (hasItem) json << ",\n";
    json << "    {\"role\": \"user\", \"parts\": [{\"text\": \"" << jsonEscape(userMessage) << "\"}]}\n";
    json << "  ]\n";
    json << "}";
    return json.str();
}

// ============================================================
//  3️⃣ Gemini API 呼び出し（WinINet HTTPS POST）
// ============================================================
std::string callGeminiApi(const std::string& requestBody) {
    if (Google_API_Key.empty()) {
        return "エラー: .env から APIキー (Google_API_Key) が読み込めませんでした。";
    }

    // ★ Google API推奨の gemini-3.8-flash モデルを使用
    std::string path = "/v1beta/models/gemini-3.8-flash:generateContent?key=" + Google_API_Key;

    HINTERNET hSession = InternetOpenA("GeminiChatApp", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hSession) return "エラー: インターネットセッションの初期化に失敗しました。";

    // ★ 修正: サーバー名を正しく指定
    HINTERNET hConnect = InternetConnectA(hSession, "generativelanguage.googleapis.com",
        INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
        InternetCloseHandle(hSession);
        return "エラー: サーバーへの接続に失敗しました。";
    }

    HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", path.c_str(), nullptr, nullptr, nullptr,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0);
    if (!hRequest) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hSession);
        return "エラー: リクエストの作成に失敗しました。";
    }

    std::string headers = "Content-Type: application/json\r\n";
    BOOL bSend = HttpSendRequestA(hRequest, headers.c_str(), (DWORD)headers.length(),
        (LPVOID)requestBody.c_str(), (DWORD)requestBody.length());

    std::string responseData;
    if (bSend) {
        char buf[4096];
        DWORD bytesRead = 0;
        while (InternetReadFile(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
            responseData.append(buf, bytesRead);
        }
    } else {
        responseData = "エラー: APIへのリクエスト送信に失敗しました。";
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hSession);

    return responseData;
}

// ============================================================
//  APIレスポンスからテキスト部分を抽出
// ============================================================
std::string extractReplyText(const std::string& raw) {
    // "text": "..." の最初の出現を探す（簡易パーサー）
    std::string marker = "\"text\": \"";
    auto pos = raw.find(marker);
    if (pos == std::string::npos) {
        marker = "\"text\":\"";
        pos = raw.find(marker);
    }
    if (pos == std::string::npos) {
        // エラーメッセージを探す
        std::string errMarker = "\"message\": \"";
        auto errPos = raw.find(errMarker);
        if (errPos == std::string::npos) {
            errMarker = "\"message\":\"";
            errPos = raw.find(errMarker);
        }
        if (errPos != std::string::npos) {
            size_t start = errPos + errMarker.size();
            size_t end = raw.find("\"", start);
            if (end != std::string::npos) {
                return "Google API エラー: " + raw.substr(start, end - start);
            }
        }
        return "エラー: APIからの応答を解析できませんでした。";
    }

    size_t start = pos + marker.size();
    std::string result;
    for (size_t i = start; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == '"') { result += '"'; ++i; }
            else if (next == 'n') { result += '\n'; ++i; }
            else if (next == 'r') { result += '\r'; ++i; }
            else if (next == 't') { result += '\t'; ++i; }
            else if (next == '\\') { result += '\\'; ++i; }
            else { result += raw[i]; }
        } else if (raw[i] == '"') {
            break;
        } else {
            result += raw[i];
        }
    }
    return result;
}

// ============================================================
//  会話履歴の更新
// ============================================================
void appendToHistory(const std::string& historyPath, const std::string& userMsg, const std::string& botReply) {
    std::string history = readHistory(historyPath);
    // 末尾の ] を除去
    if (history.size() >= 1 && history.back() == ']') {
        history.pop_back();
    }
    while (!history.empty() && std::isspace((unsigned char)history.back())) {
        history.pop_back();
    }

    std::ostringstream entry;
    bool isEmpty = (history == "[" || history.empty());
    if (!isEmpty) {
        entry << ",\n";
    }
    entry << "    {\"role\": \"user\",  \"parts\": [{\"text\": \"" << jsonEscape(userMsg) << "\"}]},\n";
    entry << "    {\"role\": \"model\", \"parts\": [{\"text\": \"" << jsonEscape(botReply) << "\"}]}\n";
    entry << "]";

    if (history.empty()) history = "[";
    writeHistory(historyPath, history + entry.str());
}

// ============================================================
//  ワーカースレッド関数
// ============================================================
void GeminiThreadProc(ThreadData* data) {
    std::string rawResponse = callGeminiApi(data->requestJson);
    std::string replyText = extractReplyText(rawResponse);

    // 会話履歴に追加
    appendToHistory(g_historyPath, data->userMessage, replyText);

    // UIスレッドに返信テキストを送る
    BSTR bstrResponse = SysAllocString(utf8ToWide(replyText).c_str());
    PostMessage(g_hwnd, WM_GEMINI_REPLY, (WPARAM)bstrResponse, 0);

    delete data;
}

// ============================================================
//  COM コールバックハンドラ（MinGW互換）
//  Microsoft::WRL::Callback はMinGWで使えないので手動実装
// ============================================================

// --- WebMessageReceived ハンドラ ---
class WebMessageReceivedHandler : public ICoreWebView2WebMessageReceivedEventHandler {
    ULONG m_refCount = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR msgRaw = nullptr;
        args->TryGetWebMessageAsString(&msgRaw);
        if (msgRaw) {
            std::string userMsg = wideToUtf8(msgRaw);
            CoTaskMemFree(msgRaw);

            // リクエストJSON構築
            std::string reqJson = buildRequestJson(userMsg, g_historyPath);

            // ワーカースレッドでAPI呼び出し
            ThreadData* td = new ThreadData();
            td->userMessage = userMsg;
            td->requestJson = reqJson;
            std::thread(GeminiThreadProc, td).detach();
        }
        return S_OK;
    }
};

// --- CreateCoreWebView2Controller 完了ハンドラ ---
class ControllerCompletedHandler : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
    ULONG m_refCount = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        if (FAILED(result) || !controller) {
            MessageBoxA(g_hwnd, "WebView2 コントローラーの作成に失敗しました。", "エラー", MB_OK | MB_ICONERROR);
            return S_OK;
        }

        g_controller = controller;
        g_controller->AddRef();

        g_controller->get_CoreWebView2(&g_webview);

        // ウィンドウサイズに合わせる
        RECT bounds;
        GetClientRect(g_hwnd, &bounds);
        g_controller->put_Bounds(bounds);

        // メッセージ受信ハンドラを登録
        EventRegistrationToken token;
        g_webview->add_WebMessageReceived(new WebMessageReceivedHandler(), &token);

        // index.html をロード（Unicode パスで日本語フォルダ対応）
        std::wstring htmlPath = getExeDirW() + L"ui\\index.html";
        g_webview->Navigate(htmlPath.c_str());

        return S_OK;
    }
};

// --- CreateCoreWebView2Environment 完了ハンドラ ---
class EnvironmentCompletedHandler : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
    ULONG m_refCount = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        if (FAILED(result) || !env) {
            MessageBoxA(g_hwnd, "WebView2 環境の作成に失敗しました。", "エラー", MB_OK | MB_ICONERROR);
            return S_OK;
        }
        env->CreateCoreWebView2Controller(g_hwnd, new ControllerCompletedHandler());
        return S_OK;
    }
};

// ============================================================
//  ウィンドウプロシージャ
// ============================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_controller) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        return 0;

    case WM_GEMINI_REPLY: {
        BSTR bstr = (BSTR)wp;
        if (bstr && g_webview) {
            // JavaScript で bot の吹き出しを表示
            std::wstring reply(bstr, SysStringLen(bstr));

            // JS文字列用にエスケープ
            std::wstring escaped;
            for (wchar_t ch : reply) {
                if (ch == L'\\') escaped += L"\\\\";
                else if (ch == L'\'') escaped += L"\\'";
                else if (ch == L'\n') escaped += L"\\n";
                else if (ch == L'\r') escaped += L"\\r";
                else escaped += ch;
            }

            std::wstring js = L"addMessage('" + escaped + L"', 'bot');";
            g_webview->ExecuteScript(js.c_str(), nullptr);
        }
        if (bstr) SysFreeString(bstr);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
//  WebView2 初期化（DLL動的ロード — MinGWでは.libリンク不可のため）
// ============================================================
typedef HRESULT(STDAPICALLTYPE* FnCreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

bool InitWebView2() {
    HMODULE hLib = LoadLibraryA("WebView2Loader.dll");
    if (!hLib) {
        MessageBoxA(g_hwnd,
            "WebView2Loader.dll が見つかりません。\n"
            "exe と同じフォルダに配置してください。",
            "エラー", MB_OK | MB_ICONERROR);
        return false;
    }

    auto fnCreate = (FnCreateCoreWebView2EnvironmentWithOptions)
        GetProcAddress(hLib, "CreateCoreWebView2EnvironmentWithOptions");

    if (!fnCreate) {
        MessageBoxA(g_hwnd, "CreateCoreWebView2EnvironmentWithOptions が見つかりません。", "エラー", MB_OK | MB_ICONERROR);
        return false;
    }

    HRESULT hr = fnCreate(nullptr, nullptr, nullptr, new EnvironmentCompletedHandler());
    return SUCCEEDED(hr);
}

// ============================================================
//  WinMain — エントリーポイント
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    try {
        // COM 初期化
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        // exe のあるディレクトリを取得
        g_exePath = getExeDir();
        g_historyPath = g_exePath + "history.json";

        // .env 読み込み ★★★
        loadEnv(g_exePath + ".env");

        // 会話履歴リセット（起動毎にクリア）
        resetHistory(g_historyPath);

        // ウィンドウクラス登録
        const wchar_t CLASS_NAME[] = L"GmaiChatWindow";
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = WndProc;
        wc.hInstance      = hInstance;
        wc.lpszClassName  = CLASS_NAME;
        wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);

        // ウィンドウ作成
        g_hwnd = CreateWindowExW(
            0, CLASS_NAME,
            L"Gmai  AIチャット",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
            nullptr, nullptr, hInstance, nullptr);

        if (!g_hwnd) {
            MessageBoxA(nullptr, "ウィンドウの作成に失敗しました。", "エラー", MB_OK | MB_ICONERROR);
            return 1;
        }

        ShowWindow(g_hwnd, nCmdShow);
        UpdateWindow(g_hwnd);

        // WebView2 初期化
        if (!InitWebView2()) {
            MessageBoxA(nullptr, "WebView2 の初期化に失敗しました。", "エラー", MB_OK | MB_ICONERROR);
            return 1;
        }

        // メッセージループ
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        CoUninitialize();
        return (int)msg.wParam;

    } catch (...) {
        MessageBoxA(nullptr,
            "予期しないエラーが発生しました。\nアプリを再起動してください。",
            "致命的エラー", MB_OK | MB_ICONERROR);
        return 1;
    }
}
