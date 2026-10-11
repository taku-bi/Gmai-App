// ============================================================
//  main.cpp — Gmai チャットアプリ（WebView2 + Gemini API）
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <wrl.h>

// WebView2 SDK ヘッダー
#include "webview2/WebView2.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <thread>

#pragma comment(lib, "wininet.lib")

using namespace Microsoft::WRL;

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
// ★【APIキー変数】
// Google_API_Key の中身は実行時に .env から読み込まれます。
// コード内にキー文字列を直接代入・ハードコードしません。
// ------------------------------------------------------------
static std::string Google_API_Key;

static std::string g_systemInstruction;
static std::string g_familyContext;
static std::string g_exePath;   // exe のあるディレクトリ

// ============================================================
//  ユーティリティ: exe のディレクトリパスを取得
// ============================================================
std::string getExeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
    return path.substr(0, path.find_last_of("\\/") + 1);
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

// 文字列置換ユーティリティ
std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
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

        if (value.size() >= 2 &&
            ((value.front() == '"'  && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

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
    std::filesystem::remove(path);
    std::ofstream file(path);
    file << "[]";
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

std::string buildRequestJson(const std::string& userMessage, const std::string& historyPath) {
    std::string history = readHistory(historyPath);
    std::ostringstream json;
    json << "{\n";
    json << "  \"system_instruction\": {\n";
    json << "    \"parts\": [{\"text\": \"" << jsonEscape(g_systemInstruction) << "\"}]\n";
    json << "  },\n";
    json << "  \"contents\": [\n";
    json << "    {\"role\": \"user\",  \"parts\": [{\"text\": \"" << jsonEscape(g_familyContext) << "\"}]},\n";
    json << "    {\"role\": \"model\", \"parts\": [{\"text\": \"わかりました。\"}]}";

    if (history.size() > 2) {
        std::string inner = history.substr(1, history.size() - 2);
        while (!inner.empty() && std::isspace((unsigned char)inner.front())) inner.erase(inner.begin());
        while (!inner.empty() && std::isspace((unsigned char)inner.back())) inner.pop_back();
        if (!inner.empty()) {
            json << ",\n    " << inner;
        }
    }
    json << ",\n    {\"role\": \"user\", \"parts\": [{\"text\": \"" << jsonEscape(userMessage) << "\"}]}\n";
    json << "  ]\n";
    json << "}";
    return json.str();
}

// ============================================================
//  3️⃣ Gemini API 呼び出し（WinINet HTTPS POST）
// ============================================================
std::string callGeminiApi(const std::string& requestBody) {
    const std::string host = "generativelanguage.googleapis.com";

    // ------------------------------------------------------------
    // ★★★【Google_API_Key の使用箇所】★★★
    // クエリパラメータ ?key= に Google_API_Key を付与してリクエストURLを生成
    // ------------------------------------------------------------
    std::string url_path = "/v1beta/models/gemini-2.0-flash:generateContent?key=" + Google_API_Key;
    std::string response_data = "";

    HINTERNET hInternet = InternetOpenA("ParentAIChat", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (hInternet) {
        HINTERNET hConnect = InternetConnectA(hInternet, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        if (hConnect) {
            HINTERNET hRequest = HttpOpenRequestA(hConnect, "POST", url_path.c_str(), NULL, NULL, NULL, INTERNET_FLAG_SECURE, 0);
            if (hRequest) {
                std::string headers = "Content-Type: application/json\r\n";
                if (HttpSendRequestA(hRequest, headers.c_str(), (DWORD)headers.length(), (LPVOID)requestBody.c_str(), (DWORD)requestBody.length())) {
                    char buffer[4096];
                    DWORD bytesRead = 0;
                    while (InternetReadFile(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
                        buffer[bytesRead] = '\0';
                        response_data += buffer;
                    }
                }
                InternetCloseHandle(hRequest);
            }
            InternetCloseHandle(hConnect);
        }
        InternetCloseHandle(hInternet);
    }

    // JSONから返答テキスト（"text": "..."）を簡易的に抽出
    size_t text_pos = response_data.find("\"text\": \"");
    if (text_pos != std::string::npos) {
        size_t start = text_pos + 9;
        size_t end = response_data.find("\"", start);
        if (end != std::string::npos) {
            std::string reply = response_data.substr(start, end - start);
            reply = replaceAll(reply, "\\n", "\n");
            reply = replaceAll(reply, "\\\"", "\"");
            return reply;
        }
    }
    return "お母さん、いまちょっと手が離せないみたい（通信エラー）。";
}

// 履歴の上書き蓄積
void appendToHistory(const std::string& user_msg, const std::string& ai_reply, const std::string& path) {
    std::string history = readHistory(path);
    if (history.size() < 2) history = "[]";
    std::string contents = history.substr(0, history.length() - 1);
    while (!contents.empty() && (contents.back() == ' ' || contents.back() == '\n' || contents.back() == '\r')) {
        contents.pop_back();
    }
    if (contents.length() > 1 && contents.back() != '[') contents += ",";

    contents += "{\"role\": \"user\", \"parts\": [{\"text\": \"" + jsonEscape(user_msg) + "\"}]},"
                "{\"role\": \"model\", \"parts\": [{\"text\": \"" + jsonEscape(ai_reply) + "\"}]}]";
    writeHistory(path, contents);
}

// 4️⃣ UIスレッドをフリーズさせないための非同期バックグラウンド処理（ワーカースレッド）
void geminiThreadWorker(ThreadData* data) {
    std::string ai_reply = callGeminiApi(data->requestJson);
    appendToHistory(data->userMessage, ai_reply, g_exePath + "history.json");

    // UIスレッドへ返答を通知
    std::wstring* wreply = new std::wstring(utf8ToWide(ai_reply));
    PostMessage(g_hwnd, WM_GEMINI_REPLY, 0, reinterpret_cast<LPARAM>(wreply));
    delete data;
}

void onUserMessage(const std::wstring& message) {
    std::string userText = wideToUtf8(message);
    std::string historyPath = g_exePath + "history.json";
    std::string reqJson = buildRequestJson(userText, historyPath);

    ThreadData* data = new ThreadData{ userText, reqJson };
    std::thread t(geminiThreadWorker, data);
    t.detach();
}

// ============================================================
//  WebView2 の初期化
// ============================================================
void initWebView2() {
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr)) return hr;

                env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(hr)) return hr;

                            g_controller = controller;
                            g_controller->AddRef();
                            g_controller->get_CoreWebView2(&g_webview);

                            RECT bounds;
                            GetClientRect(g_hwnd, &bounds);
                            g_controller->put_Bounds(bounds);

                            std::wstring htmlPath = utf8ToWide(g_exePath) + L"ui\\index.html";
                            std::wstring url = L"file:///" + htmlPath;
                            std::replace(url.begin(), url.end(), L'\\', L'/');
                            g_webview->Navigate(url.c_str());

                            EventRegistrationToken token;
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* /*webview*/,
                                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msg = nullptr;
                                        args->TryGetWebMessageAsString(&msg);
                                        if (msg) {
                                            std::wstring message(msg);
                                            CoTaskMemFree(msg);
                                            onUserMessage(message);
                                        }
                                        return S_OK;
                                    }
                                ).Get(),
                                &token
                            );

                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );
}

// ============================================================
//  ウィンドウプロシージャ
// ============================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (g_controller) {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                g_controller->put_Bounds(bounds);
            }
            return 0;

        case WM_GEMINI_REPLY: {
            std::wstring* reply = reinterpret_cast<std::wstring*>(lParam);
            if (g_webview && reply) {
                g_webview->PostWebMessageAsString(reply->c_str());
            }
            delete reply;
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
//  エントリーポイント
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_exePath = getExeDir();

    loadEnv(g_exePath + ".env");
    resetHistory(g_exePath + "history.json");

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"GmaiAppClass";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"GmaiAppClass", L"Gmai - AIチャット",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 800,
        nullptr, nullptr, hInstance, nullptr);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    initWebView2();

    MSG winMsg;
    while (GetMessage(&winMsg, nullptr, 0, 0)) {
        TranslateMessage(&winMsg);
        DispatchMessage(&winMsg);
    }

    if (g_webview)    g_webview->Release();
    if (g_controller) g_controller->Release();
    CoUninitialize();

    return (int)winMsg.wParam;
}