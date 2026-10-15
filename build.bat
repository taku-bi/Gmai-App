@echo off
chcp 65001 > nul
echo 🔨 ビルドを開始します...

g++ -std=c++17 -mwindows src\main.cpp src\webview2\WebView2Loader.dll.lib -Isrc -lwininet -lole32 -o main.exe
if %ERRORLEVEL% NEQ 0 (
    echo ❌ ビルドに失敗しました。
    exit /b %ERRORLEVEL%
)

echo 📦 Gmail_App フォルダを更新中...
if not exist Gmail_App\ui mkdir Gmail_App\ui
copy /Y main.exe Gmail_App\ > nul
copy /Y src\webview2\WebView2Loader.dll Gmail_App\ > nul
copy /Y .env Gmail_App\ > nul
copy /Y ui\* Gmail_App\ui\ > nul

echo ✨ ビルドと配置が完了しました！
echo 🚀 起動するには: start Gmail_App\main.exe

